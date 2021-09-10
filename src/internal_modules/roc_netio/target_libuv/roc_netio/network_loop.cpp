/*
 * Copyright (c) 2015 Roc Streaming authors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "roc_netio/network_loop.h"
#include "roc_address/socket_addr_to_str.h"
#include "roc_core/helpers.h"
#include "roc_core/log.h"
#include "roc_core/panic.h"
#include "roc_core/shared_ptr.h"

namespace roc {
namespace netio {

NetworkLoop::Task::Task()
    : func_(NULL)
    , state_(Initialized)
    , success_(false)
    , port_handle_(NULL)
    , handler_(NULL) {
}

NetworkLoop::Task::~Task() {
    if (state_ != Finished) {
        roc_panic("network loop: attemp to destroy task before it's finished");
    }
}

bool NetworkLoop::Task::success() const {
    return state_ == Finished && success_;
}

NetworkLoop::Tasks::AddUdpReceiverPort::AddUdpReceiverPort(UdpReceiverConfig& config,
                                                           packet::IWriter& writer) {
    func_ = &NetworkLoop::task_add_udp_receiver_;
    config_ = &config;
    writer_ = &writer;
}

NetworkLoop::PortHandle NetworkLoop::Tasks::AddUdpReceiverPort::get_handle() const {
    if (!success()) {
        return NULL;
    }
    roc_panic_if_not(port_handle_);
    return port_handle_;
}

NetworkLoop::Tasks::AddUdpSenderPort::AddUdpSenderPort(UdpSenderConfig& config) {
    func_ = &NetworkLoop::task_add_udp_sender_;
    config_ = &config;
    writer_ = NULL;
}

NetworkLoop::PortHandle NetworkLoop::Tasks::AddUdpSenderPort::get_handle() const {
    if (!success()) {
        return NULL;
    }
    roc_panic_if_not(port_handle_);
    return port_handle_;
}

packet::IWriter* NetworkLoop::Tasks::AddUdpSenderPort::get_writer() const {
    if (!success()) {
        return NULL;
    }
    roc_panic_if_not(writer_);
    return writer_;
}

NetworkLoop::Tasks::AddTcpServerPort::AddTcpServerPort(TcpServerConfig& config,
                                                       IConnAcceptor& conn_acceptor) {
    func_ = &NetworkLoop::task_add_tcp_server_;
    config_ = &config;
    conn_acceptor_ = &conn_acceptor;
}

NetworkLoop::PortHandle NetworkLoop::Tasks::AddTcpServerPort::get_handle() const {
    if (!success()) {
        return NULL;
    }
    roc_panic_if_not(port_handle_);
    return port_handle_;
}

NetworkLoop::Tasks::AddTcpClientPort::AddTcpClientPort(TcpClientConfig& config,
                                                       IConnHandler& conn_handler) {
    func_ = &NetworkLoop::task_add_tcp_client_;
    config_ = &config;
    conn_handler_ = &conn_handler;
}

NetworkLoop::PortHandle NetworkLoop::Tasks::AddTcpClientPort::get_handle() const {
    if (!success()) {
        return NULL;
    }
    roc_panic_if_not(port_handle_);
    return port_handle_;
}

NetworkLoop::Tasks::RemovePort::RemovePort(PortHandle handle) {
    func_ = &NetworkLoop::task_remove_port_;
    if (!handle) {
        roc_panic("network loop: handle is null");
    }
    port_ = (BasicPort*)handle;
}

NetworkLoop::Tasks::ResolveEndpointAddress::ResolveEndpointAddress(
    const address::EndpointURI& endpoint_uri) {
    func_ = &NetworkLoop::task_resolve_endpoint_address_;
    resolve_req_.endpoint_uri = &endpoint_uri;
}

const address::SocketAddr&
NetworkLoop::Tasks::ResolveEndpointAddress::get_address() const {
    return resolve_req_.resolved_address;
}

NetworkLoop::ICompletionHandler::~ICompletionHandler() {
}

NetworkLoop::NetworkLoop(packet::PacketPool& packet_pool,
                         core::BufferPool<uint8_t>& buffer_pool,
                         core::IAllocator& allocator)
    : packet_pool_(packet_pool)
    , buffer_pool_(buffer_pool)
    , allocator_(allocator)
    , started_(false)
    , loop_initialized_(false)
    , stop_sem_initialized_(false)
    , task_sem_initialized_(false)
    , resolver_(*this, loop_)
    , num_open_ports_(0) {
    if (int err = uv_loop_init(&loop_)) {
        roc_log(LogError, "network loop: uv_loop_init(): [%s] %s", uv_err_name(err),
                uv_strerror(err));
        return;
    }
    loop_initialized_ = true;

    if (int err = uv_async_init(&loop_, &stop_sem_, stop_sem_cb_)) {
        roc_log(LogError, "network loop: uv_async_init(): [%s] %s", uv_err_name(err),
                uv_strerror(err));
        return;
    }
    stop_sem_.data = this;
    stop_sem_initialized_ = true;

    if (int err = uv_async_init(&loop_, &task_sem_, task_sem_cb_)) {
        roc_log(LogError, "network loop: uv_async_init(): [%s] %s", uv_err_name(err),
                uv_strerror(err));
        return;
    }
    task_sem_.data = this;
    task_sem_initialized_ = true;

    started_ = Thread::start();
}

NetworkLoop::~NetworkLoop() {
    if (started_) {
        if (int err = uv_async_send(&stop_sem_)) {
            roc_panic("network loop: uv_async_send(): [%s] %s", uv_err_name(err),
                      uv_strerror(err));
        }
    } else {
        close_all_sems_();
    }

    if (loop_initialized_) {
        if (started_) {
            Thread::join();
        } else {
            // If the thread was never started we should manually run the loop to
            // wait all opened handles to be closed. Otherwise, uv_loop_close()
            // will fail with EBUSY.
            NetworkLoop::run(); // non-virtual call from dtor
        }

        if (int err = uv_loop_close(&loop_)) {
            roc_panic("network loop: uv_loop_close(): [%s] %s", uv_err_name(err),
                      uv_strerror(err));
        }
    }

    roc_panic_if(joinable());
    roc_panic_if(open_ports_.size());
    roc_panic_if(closing_ports_.size());
    roc_panic_if(task_sem_initialized_);
    roc_panic_if(stop_sem_initialized_);
}

bool NetworkLoop::valid() const {
    return started_;
}

size_t NetworkLoop::num_ports() const {
    return (size_t)num_open_ports_;
}

void NetworkLoop::schedule(Task& task, ICompletionHandler& handler) {
    if (!valid()) {
        roc_panic("network loop: can't use invalid loop");
    }

    if (task.state_ != Task::Initialized) {
        roc_panic("network loop: can't use the same task multiple times");
    }

    task.handler_ = &handler;
    task.state_ = Task::Pending;

    pending_tasks_.push_back(task);

    if (int err = uv_async_send(&task_sem_)) {
        roc_panic("network loop: uv_async_send(): [%s] %s", uv_err_name(err),
                  uv_strerror(err));
    }
}

bool NetworkLoop::schedule_and_wait(Task& task) {
    if (!valid()) {
        roc_panic("network loop: can't use invalid loop");
    }

    if (task.state_ != Task::Initialized) {
        roc_panic("network loop: can't use the same task multiple times");
    }

    if (!task.sem_) {
        task.sem_.reset(new (task.sem_) core::Semaphore);
    }

    task.handler_ = NULL;
    task.state_ = Task::Pending;

    pending_tasks_.push_back(task);

    if (int err = uv_async_send(&task_sem_)) {
        roc_panic("network loop: uv_async_send(): [%s] %s", uv_err_name(err),
                  uv_strerror(err));
    }

    task.sem_->wait();

    return task.success_;
}

void NetworkLoop::handle_terminate_completed(IConn& conn, void* arg) {
    core::SharedPtr<TcpConnectionPort> port(static_cast<TcpConnectionPort*>(&conn));

    if (!closing_ports_.contains(*port)) {
        roc_panic("network loop: port is not in closing ports list: %s",
                  port->descriptor());
    }

    roc_log(LogDebug, "network loop: asynchronous terminate finished: port %s",
            port->descriptor());

    Task* task = (Task*)arg;

    if (async_close_port_(port, task) == AsyncOp_Started) {
        roc_log(LogDebug, "network loop: initiated asynchronous close: port %s",
                port->descriptor());
    } else {
        roc_log(LogDebug, "network loop: closed port: port %s", port->descriptor());

        finish_closing_port_(port, task);
    }
}

void NetworkLoop::handle_close_completed(BasicPort& port_ref, void* arg) {
    core::SharedPtr<BasicPort> port(static_cast<BasicPort*>(&port_ref));

    if (!closing_ports_.contains(*port)) {
        roc_panic("network loop: port is not in closing ports list: %s",
                  port->descriptor());
    }

    roc_log(LogDebug, "network loop: asynchronous close finished: port %s",
            port->descriptor());

    finish_closing_port_(port, (Task*)arg);
}

void NetworkLoop::handle_resolved(ResolverRequest& req) {
    Tasks::ResolveEndpointAddress& task =
        *ROC_CONTAINER_OF(&req, Tasks::ResolveEndpointAddress, resolve_req_);

    task.success_ = req.success;
    finish_task_(task);
}

void NetworkLoop::run() {
    roc_log(LogDebug, "network loop: starting event loop");

    int err = uv_run(&loop_, UV_RUN_DEFAULT);
    if (err != 0) {
        roc_log(LogInfo, "network loop: uv_run() returned non-zero");
    }

    roc_log(LogDebug, "network loop: finishing event loop");
}

void NetworkLoop::task_sem_cb_(uv_async_t* handle) {
    roc_panic_if_not(handle);

    NetworkLoop& self = *(NetworkLoop*)handle->data;
    self.process_pending_tasks_();
}

void NetworkLoop::stop_sem_cb_(uv_async_t* handle) {
    roc_panic_if_not(handle);

    NetworkLoop& self = *(NetworkLoop*)handle->data;
    self.close_all_ports_();
    self.close_all_sems_();
    self.process_pending_tasks_();
}

void NetworkLoop::process_pending_tasks_() {
    // Using try_pop_front_exclusive() makes this method lock-free and wait-free.
    // try_pop_front_exclusive() may return NULL if the queue is not empty, but
    // push_back() is currently in progress. In this case we can exit the loop
    // before processing all tasks, but schedule() always calls uv_async_send()
    // after push_back(), so we'll wake up soon and process the rest tasks.
    while (Task* task = pending_tasks_.try_pop_front_exclusive()) {
        (this->*(task->func_))(*task);

        if (task->state_ == Task::Finishing) {
            finish_task_(*task);
        }
    }
}

void NetworkLoop::finish_task_(Task& task) {
    ICompletionHandler* handler = task.handler_;

    task.state_ = Task::Finished;

    if (handler) {
        handler->network_task_finished(task);
    } else {
        task.sem_->post();
    }
}

void NetworkLoop::async_terminate_conn_port_(
    const core::SharedPtr<TcpConnectionPort>& port, Task* task) {
    closing_ports_.push_back(*port);

    port->attach_terminate_handler(*this, task);
    port->async_terminate(Term_Failure);
}

AsyncOperationStatus
NetworkLoop::async_close_port_(const core::SharedPtr<BasicPort>& port, Task* task) {
    const AsyncOperationStatus status = port->async_close(*this, task);

    if (status == AsyncOp_Started) {
        if (!closing_ports_.contains(*port)) {
            closing_ports_.push_back(*port);
        }
    }

    return status;
}

void NetworkLoop::finish_closing_port_(const core::SharedPtr<BasicPort>& port,
                                       Task* task) {
    closing_ports_.remove(*port);

    if (task) {
        finish_task_(*task);
    }
}

void NetworkLoop::update_num_ports_() {
    num_open_ports_ = (int)open_ports_.size();
}

void NetworkLoop::close_all_ports_() {
    while (core::SharedPtr<BasicPort> port = open_ports_.front()) {
        open_ports_.remove(*port);
        async_close_port_(port, NULL);
    }
    update_num_ports_();
}

void NetworkLoop::close_all_sems_() {
    if (task_sem_initialized_) {
        uv_close((uv_handle_t*)&task_sem_, NULL);
        task_sem_initialized_ = false;
    }

    if (stop_sem_initialized_) {
        uv_close((uv_handle_t*)&stop_sem_, NULL);
        stop_sem_initialized_ = false;
    }
}

void NetworkLoop::task_add_udp_receiver_(Task& base_task) {
    Tasks::AddUdpReceiverPort& task = (Tasks::AddUdpReceiverPort&)base_task;

    core::SharedPtr<UdpReceiverPort> port = new (allocator_) UdpReceiverPort(
        *task.config_, *task.writer_, loop_, packet_pool_, buffer_pool_, allocator_);
    if (!port) {
        roc_log(
            LogError,
            "network loop: can't add udp receiver port %s: can't allocate udp receiver",
            address::socket_addr_to_str(task.config_->bind_address).c_str());
        task.success_ = false;
        task.state_ = Task::Finishing;
        return;
    }

    task.port_ = port;

    if (!port->open()) {
        roc_log(LogError,
                "network loop: can't add udp receiver port %s: can't start udp receiver",
                address::socket_addr_to_str(task.config_->bind_address).c_str());
        task.success_ = false;
        if (async_close_port_(port, &task) == AsyncOp_Started) {
            task.state_ = Task::ClosingPort;
        } else {
            task.state_ = Task::Finishing;
        }
        return;
    }

    open_ports_.push_back(*port);
    update_num_ports_();

    task.config_->bind_address = port->bind_address();
    task.port_handle_ = (PortHandle)port.get();

    task.success_ = true;
    task.state_ = Task::Finishing;
}

void NetworkLoop::task_add_udp_sender_(Task& base_task) {
    Tasks::AddUdpSenderPort& task = (Tasks::AddUdpSenderPort&)base_task;

    core::SharedPtr<UdpSenderPort> port =
        new (allocator_) UdpSenderPort(*task.config_, loop_, allocator_);
    if (!port) {
        roc_log(LogError,
                "network loop: can't add udp sender port %s: can't allocate udp sender",
                address::socket_addr_to_str(task.config_->bind_address).c_str());
        task.success_ = false;
        task.state_ = Task::Finishing;
        return;
    }

    task.port_ = port;

    if (!port->open()) {
        roc_log(LogError,
                "network loop: can't add udp sebder port %s: can't start udp sender",
                address::socket_addr_to_str(task.config_->bind_address).c_str());
        task.success_ = false;
        if (async_close_port_(port, &task) == AsyncOp_Started) {
            task.state_ = Task::ClosingPort;
        } else {
            task.state_ = Task::Finishing;
        }
        return;
    }

    open_ports_.push_back(*port);
    update_num_ports_();

    task.config_->bind_address = port->bind_address();
    task.port_handle_ = (PortHandle)port.get();
    task.writer_ = port.get();

    task.success_ = true;
    task.state_ = Task::Finishing;
}

void NetworkLoop::task_add_tcp_server_(Task& base_task) {
    Tasks::AddTcpServerPort& task = (Tasks::AddTcpServerPort&)base_task;

    core::SharedPtr<TcpServerPort> port = new (allocator_)
        TcpServerPort(*task.config_, *task.conn_acceptor_, loop_, allocator_);
    if (!port) {
        roc_log(LogError,
                "network loop: can't add tcp server port %s: can't allocate tcp server",
                address::socket_addr_to_str(task.config_->bind_address).c_str());
        task.success_ = false;
        task.state_ = Task::Finishing;
        return;
    }

    task.port_ = port;

    if (!port->open()) {
        roc_log(LogError,
                "network loop: can't add tcp server port %s: can't start tcp server",
                address::socket_addr_to_str(task.config_->bind_address).c_str());
        task.success_ = false;
        if (async_close_port_(port, &task) == AsyncOp_Started) {
            task.state_ = Task::ClosingPort;
        } else {
            task.state_ = Task::Finishing;
        }
        return;
    }

    open_ports_.push_back(*port);
    update_num_ports_();

    task.config_->bind_address = port->bind_address();
    task.port_handle_ = (PortHandle)port.get();

    task.success_ = true;
    task.state_ = Task::Finishing;
}

void NetworkLoop::task_add_tcp_client_(Task& base_task) {
    Tasks::AddTcpClientPort& task = (Tasks::AddTcpClientPort&)base_task;

    core::SharedPtr<TcpConnectionPort> port =
        new (allocator_) TcpConnectionPort(TcpConn_Client, loop_, allocator_);
    if (!port) {
        roc_log(LogError,
                "network loop: can't add tcp client port %s: can't allocate tcp client",
                address::socket_addr_to_str(task.config_->remote_address).c_str());
        task.success_ = false;
        task.state_ = Task::Finishing;
        return;
    }

    task.port_ = port;

    if (!port->open()) {
        roc_log(LogError,
                "network loop: can't add tcp client port %s: can't start tcp client",
                address::socket_addr_to_str(task.config_->remote_address).c_str());
        task.success_ = false;
        if (async_close_port_(port, &task) == AsyncOp_Started) {
            task.state_ = Task::ClosingPort;
        } else {
            task.state_ = Task::Finishing;
        }
        return;
    }

    if (!port->connect(*task.config_)) {
        roc_log(LogError,
                "network loop: can't add tcp client port %s: can't start tcp client",
                address::socket_addr_to_str(task.config_->remote_address).c_str());
        task.success_ = false;
        task.state_ = Task::ClosingPort;
        async_terminate_conn_port_(port, &task);
        return;
    }

    port->attach_connection_handler(*task.conn_handler_);

    open_ports_.push_back(*port);
    update_num_ports_();

    task.config_->local_address = port->local_address();
    task.port_handle_ = (PortHandle)port.get();

    task.success_ = true;
    task.state_ = Task::Finishing;
}

void NetworkLoop::task_remove_port_(Task& base_task) {
    Tasks::RemovePort& task = (Tasks::RemovePort&)base_task;

    roc_log(LogDebug, "network loop: removing port %s", task.port_->descriptor());

    open_ports_.remove(*task.port_);
    update_num_ports_();

    task.success_ = true;
    if (async_close_port_(task.port_, &task) == AsyncOp_Started) {
        task.state_ = Task::ClosingPort;
    } else {
        task.state_ = Task::Finishing;
    }
}

void NetworkLoop::task_resolve_endpoint_address_(Task& base_task) {
    Tasks::ResolveEndpointAddress& task = (Tasks::ResolveEndpointAddress&)base_task;

    if (!resolver_.async_resolve(task.resolve_req_)) {
        task.success_ = task.resolve_req_.success;
        task.state_ = Task::Finishing;
        return;
    }

    task.state_ = Task::Pending;
}

} // namespace netio
} // namespace roc
