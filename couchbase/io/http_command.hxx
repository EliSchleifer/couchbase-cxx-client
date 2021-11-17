/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *   Copyright 2020-2021 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#pragma once

#include <couchbase/io/http_session.hxx>

#include <couchbase/tracing/request_tracer.hxx>
#include <couchbase/metrics/meter.hxx>

namespace couchbase::operations
{

using http_command_handler = cxx_function::unique_function<void(std::error_code, io::http_response&&)>;

template<typename Request>
struct http_command : public std::enable_shared_from_this<http_command<Request>> {
    using encoded_request_type = typename Request::encoded_request_type;
    using encoded_response_type = typename Request::encoded_response_type;
    using error_context_type = typename Request::error_context_type;
    asio::steady_timer deadline;
    asio::steady_timer retry_backoff;
    Request request;
    encoded_request_type encoded;
    tracing::request_tracer* tracer_;
    tracing::request_span* span_{ nullptr };
    metrics::meter* meter_;
    std::shared_ptr<io::http_session> session_{};
    http_command_handler handler_{};

    http_command(asio::io_context& ctx, Request req, tracing::request_tracer* tracer, metrics::meter* meter)
      : deadline(ctx)
      , retry_backoff(ctx)
      , request(req)
      , tracer_(tracer)
      , meter_(meter)
    {
    }

    void finish_dispatch(const std::string& remote_address, const std::string& local_address)
    {
        if (span_ == nullptr) {
            return;
        }
        span_->add_tag(tracing::attributes::remote_socket, remote_address);
        span_->add_tag(tracing::attributes::local_socket, local_address);
        span_->end();
        span_ = nullptr;
    }

    void start(http_command_handler&& handler)
    {
        span_ = tracer_->start_span(tracing::span_name_for_http_service(request.type), nullptr);
        span_->add_tag(tracing::attributes::service, tracing::service_name_for_http_service(request.type));
        span_->add_tag(tracing::attributes::operation_id, request.client_context_id);
        handler_ = std::move(handler);

        deadline.expires_after(request.timeout);
        deadline.async_wait([self = this->shared_from_this()](std::error_code ec) {
            if (ec == asio::error::operation_aborted) {
                return;
            }
            self->cancel();
        });
    }

    void cancel()
    {
        if (session_) {
            session_->stop();
        }
        invoke_handler(error::common_errc::unambiguous_timeout, {});
        retry_backoff.cancel();
        deadline.cancel();
    }

    void invoke_handler(std::error_code ec, io::http_response&& msg)
    {
        if (span_ != nullptr) {
            span_->end();
            span_ = nullptr;
        }
        if (handler_) {
            handler_(ec, std::move(msg));
        }
        handler_ = nullptr;
    }

    void send()
    {
        encoded.type = request.type;
        if (auto ec = request.encode_to(encoded, session_->http_context()); ec) {
            return invoke_handler(ec, {});
        }
        encoded.headers["client-context-id"] = request.client_context_id;
        LOG_TRACE(R"({} HTTP request: {}, method={}, path="{}", client_context_id="{}", timeout={}ms)",
                  session_->log_prefix(),
                  encoded.type,
                  encoded.method,
                  encoded.path,
                  request.client_context_id,
                  request.timeout.count());
        session_->write_and_subscribe(
          encoded,
          [self = this->shared_from_this(), start = std::chrono::steady_clock::now()](std::error_code ec, io::http_response&& msg) {
              if (ec == asio::error::operation_aborted) {
                  return self->invoke_handler(error::common_errc::ambiguous_timeout, std::move(msg));
              }
              static std::string meter_name = "db.couchbase.operations";
              static std::map<std::string, std::string> tags = {
                  { "db.couchbase.service", fmt::format("{}", self->request.type) },
                  { "db.operation", self->encoded.path },
              };
              if (self->meter_) {
                  self->meter_->get_value_recorder(meter_name, tags)
                    ->record_value(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
              }
              self->deadline.cancel();
              self->finish_dispatch(self->session_->remote_address(), self->session_->local_address());
              LOG_TRACE(R"({} HTTP response: {}, client_context_id="{}", status={})",
                        self->session_->log_prefix(),
                        self->request.type,
                        self->request.client_context_id,
                        msg.status_code);
              try {
                  self->invoke_handler(ec, std::move(msg));
              } catch (const priv::retry_http_request&) {
                  self->send();
              }
          });
    }

    void send_to(std::shared_ptr<io::http_session> session)
    {
        if (!handler_) {
            return;
        }
        session_ = std::move(session);
        span_->add_tag(tracing::attributes::local_id, session_->id());
        send();
    }
};

} // namespace couchbase::operations
