
#include <chrono>
#include <thread>

#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include <rdmalib/connection.hpp>
#include <rdmalib/rdmalib.hpp>
#include <rdmalib/util.hpp>

#include <rfaas/allocation.hpp>
#include <rfaas/connection.hpp>
#include <variant>

#include "manager.hpp"

namespace rfaas::executor_manager {

  constexpr int Manager::POLLING_TIMEOUT_MS;

  Manager::Manager(Settings & settings, bool skip_rm):
    _client_queue(100),
    _ids(0),
    _res_mgr_connection(nullptr),
    _state(settings.device->ip_address, settings.rdma_device_port,
        settings.device->default_receive_buffer_size, true),
    _settings(settings),
    _skip_rm(skip_rm),
    _shutdown(false)
  {
    if(!_skip_rm) {
      _res_mgr_connection = std::make_unique<ResourceManagerConnection>(
        settings.resource_manager_address,
        settings.resource_manager_port,
        settings.device->default_receive_buffer_size
      );
    }
  }

  void Manager::shutdown()
  {
    _shutdown.store(true);
  }

  void Manager::start()
  {
    if(!_skip_rm) {
      spdlog::info(
        "Connecting to resource manager at {}:{} with secret {}.",
        _settings.resource_manager_address,
        _settings.resource_manager_port,
        _settings.resource_manager_secret
      );

      rdmalib::PrivateData data;
      data.secret(_settings.resource_manager_secret);
      data.key(1);
      rdmalib::impl::expect_true(_res_mgr_connection->connect(_settings.node_name, data.data()));
    }

    _state.register_shared_queue(0);

    spdlog::info(
      "Begin listening at {}:{} and processing events!",
      _settings.device->ip_address,
      _settings.rdma_device_port
    );
    std::thread listener(&Manager::listen, this);
    std::thread rdma_poller(&Manager::poll_rdma, this);
    std::thread res_mgr_poller(&Manager::poll_res_mgr, this);

    res_mgr_poller.join();
    listener.join();
    rdma_poller.join(); 
  }

  void Manager::listen()
  {
    // FIXME: sleep when there are no clients
    while(!_shutdown.load()) {

      bool result = _state.nonblocking_poll_events(POLLING_TIMEOUT_MS);
      if(!result)
        continue;
      spdlog::debug("[Manager-listen] Polled new rdmacm event");

      auto [conn, conn_status] = _state.poll_events();
      spdlog::debug(
        "[Manager-listen] New rdmacm connection event - connection {}, status {}",
        fmt::ptr(conn), conn_status
      );
      if(conn == nullptr){
        spdlog::error("Failed connection creation");
        continue;
      }
      if(conn_status == rdmalib::ConnectionStatus::DISCONNECTED) {
        // FIXME: handle disconnect
        spdlog::debug("[Manager-listen] Disconnection on connection {}", fmt::ptr(conn));
        _client_queue.emplace(Operation::DISCONNECT, msg_t{conn});
        continue;
      }
      // When client connects, we need to fill the receive queue with work requests before
      // accepting connection. Otherwise, we could accept before we're ready to receive data.
      else if(conn_status == rdmalib::ConnectionStatus::REQUESTED) {
        spdlog::debug("[Manager-listen] Requested new connection {}, private {}", fmt::ptr(conn), conn->private_data());
        rdmalib::PrivateData<0, 0, 32> private_data{conn->private_data()};

        if (private_data.secret() > 0) {

          _client_queue.emplace(Operation::CONNECT, msg_t{conn});

        } else {
          _state.accept(conn);

          Client client{conn, _state.pd()};
          client._active = true;
          _client_queue.emplace(Operation::CONNECT, msg_t{std::move(client)});
        }

        continue;
      }
      // Allocate structures for connections with an executor.
      // For a connection with a client we don't have to do anything. 
      else if(conn_status == rdmalib::ConnectionStatus::ESTABLISHED) {

        SPDLOG_DEBUG("[Manager-listen] New established connection {} {}", fmt::ptr(conn), conn->private_data());
        continue;
      }
    }
    spdlog::info("Background thread stops waiting for rdmacm events.");
  }

  void Manager::poll_res_mgr()
  {
    // FIXME: add executors!
    typedef std::variant<Client*, ResourceManagerConnection*> conn_t;
    std::unordered_map<uint32_t, conn_t> connections;

    uint32_t id = 0;
    connections[id++] = conn_t{_res_mgr_connection.get()};

    rdmalib::Connection& conn = _res_mgr_connection->_connection.connection();

    conn.notify_events();

    while(!_shutdown.load()) {

      auto cq = conn.wait_events();
      conn.ack_events(cq, 1);

      auto wcs = _res_mgr_connection->_connection.connection().receive_wcs().poll(false);
      if(std::get<1>(wcs)) {

        for(int j = 0; j < std::get<1>(wcs); ++j) {

          auto wc = std::get<0>(wcs)[j];
          if(wc.status != 0)
            continue;
          uint64_t id = wc.wr_id;
          SPDLOG_DEBUG("Receive lease {}", _res_mgr_connection->_receive_buffer[id].lease_id);
          std::cerr << _res_mgr_connection->_receive_buffer[id].lease_id << std::endl;
          std::cerr << _res_mgr_connection->_receive_buffer[id].cores << std::endl;
          std::cerr << _res_mgr_connection->_receive_buffer[id].memory << std::endl;
        }
      }

      _res_mgr_connection->_connection.connection().receive_wcs().refill();

      conn.notify_events();
    }

    spdlog::info("Background thread stops waiting for resource manager events.");
  }

  void Manager::poll_rdma()
  {
    rdmalib::RecvWorkCompletions* recv_queue = nullptr;
    typedef std::unordered_map<uint32_t, rdmalib::RecvWorkCompletions*> exec_t;
    exec_t executors;
    //typedef std::unordered_map<uint32_t, Client> client_t;
    //client_t clients;
    // FIXME: sleep when there are no clients

    int conn_count = 0;

    while(!_shutdown.load()) {

      bool updated = false;
      std::tuple<Operation, msg_t> result;
      std::tuple<Operation, msg_t>* result_ptr;

      if(conn_count > 0) {

        result_ptr = _client_queue.peek();
        if(result_ptr) {
          updated = true;
        }

      } else {
        updated = _client_queue.wait_dequeue_timed(result, POLLING_TIMEOUT_MS * 1000);
        if(updated) {
          result_ptr = &result;
        }
      }

      if (updated) {

        if (std::get<0>(*result_ptr) == Operation::CONNECT) {

          if(std::holds_alternative<rdmalib::Connection*>(std::get<1>(*result_ptr))) {

            rdmalib::Connection* conn = std::get<rdmalib::Connection*>(std::get<1>(*result_ptr));
            if(!recv_queue) {
              recv_queue = &conn->receive_wcs();
            }

            uint32_t qp_num = conn->private_data();
            auto it = _clients.find(qp_num);
            if(it == _clients.end()) {

              SPDLOG_DEBUG("[Manager-RDMA] Rejecting executor to an unknown client {}", qp_num);
              // This operation is thread-safe
              _state.reject(conn);

            } else {

              SPDLOG_DEBUG("[Manager-RDMA] Accepted a new executor for client {}", qp_num);
              // This operation is thread-safe
              _state.accept(conn);
              (*it).second.executor->add_executor(conn);

            }

          } else {

            Client& client = std::get<Client>(std::get<1>(*result_ptr));
            if(!recv_queue) {
              recv_queue = &client.connection->receive_wcs();
            }

            _clients.emplace(std::piecewise_construct,
                            std::forward_as_tuple(client.connection->qp()->qp_num),
                            std::forward_as_tuple(std::move(client))
            );

            SPDLOG_DEBUG("[Manager-RDMA] Accepted a new client");

          }

          // FIXME: process until not empty
          conn_count++;
        } else {

          // FIXME: disconnection
          rdmalib::Connection* conn = std::get<rdmalib::Connection*>(std::get<1>(*result_ptr));
          auto it = _clients.find(conn->qp()->qp_num);
          if (it != _clients.end()) {
            spdlog::debug("[Manager] Disconnecting client");
            _clients.erase(it);
          } else {
            spdlog::debug("[Manager] Disconnecting unknown client");
          }
          //removals.push_back(client);

        }

        if(conn_count > 0) {
          _client_queue.pop();
        }

      }

      // FIXME: Move to a separate thread + read queue

      // FIXME: users sending their lease ID 
      atomic_thread_fence(std::memory_order_acquire);
      std::vector<std::unordered_map<uint32_t, Client>::iterator> removals;
      for(auto it = _clients.begin(); it != _clients.end(); ++it) {

        Client & client = it->second;
        int i = it->first;
        auto wcs = client.connection->receive_wcs().poll(false);
        if(std::get<1>(wcs)) {
          SPDLOG_DEBUG(
            "Received at {}, work completions {}",
            i, std::get<1>(wcs)
          );
          // How to connect a wr with a specific client?
          // USE THE qp_num!
          for(int j = 0; j < std::get<1>(wcs); ++j) {

            auto wc = std::get<0>(wcs)[j];
            uint64_t id = wc.wr_id;
            int16_t cores = client.allocation_requests.data()[id].cores;
            char * client_address = client.allocation_requests.data()[id].listen_address;
            int client_port = client.allocation_requests.data()[id].listen_port;

            if(cores > 0) {
              spdlog::info(
                "Client {} requests executor with {} threads, it should connect to {}:{},"
                "it should have buffer of size {}, func buffer {}, and hot timeout {}",
                i, client.allocation_requests.data()[id].cores,
                client.allocation_requests.data()[id].listen_address,
                client.allocation_requests.data()[id].listen_port,
                client.allocation_requests.data()[id].input_buf_size,
                client.allocation_requests.data()[id].func_buf_size,
                client.allocation_requests.data()[id].hot_timeout
              );

              rdmalib::PrivateData<0,0,32> data;
              data.secret(client.connection->qp()->qp_num);
              uint64_t addr = client.accounting.address(); //+ sizeof(Accounting)*i;

              // FIXME: Docker
              auto now = std::chrono::high_resolution_clock::now();
              client.executor.reset(
                ProcessExecutor::spawn(
                  client.allocation_requests.data()[id],
                  _settings.exec,
                  {
                    _settings.device->ip_address,
                    _settings.rdma_device_port,
                    data.data(), addr, client.accounting.rkey()
                  }
                )
              );
              auto end = std::chrono::high_resolution_clock::now();
              spdlog::info(
                "Client {} at {}:{} has executor with {} ID and {} cores, time {} us",
                i, client_address, client_port, client.executor->id(), cores,
                std::chrono::duration_cast<std::chrono::microseconds>(end-now).count()
              );
            } else {
              spdlog::info("Client {} disconnects", i);
              if(client.executor) {
                auto now = std::chrono::high_resolution_clock::now();
                client.allocation_time +=
                  std::chrono::duration_cast<std::chrono::microseconds>(
                    now - client.executor->_allocation_finished
                  ).count();
              }
              //client.disable(i, _accounting_data.data()[i]);
              client.disable(i);
              removals.push_back(it);
              break;
            }
          }
          //if(client.connection)
          //  client.connection->poll_wc(rdmalib::QueueType::SEND, false);
        }
        if(client.active()) {
          client.connection->receive_wcs().refill();
          if(client.executor) {
            auto status = client.executor->check();
            if(std::get<0>(status) != ActiveExecutor::Status::RUNNING) {
              auto now = std::chrono::high_resolution_clock::now();
              client.allocation_time +=
                std::chrono::duration_cast<std::chrono::microseconds>(
                  now - client.executor->_allocation_finished
                ).count();
              // FIXME: update global manager
              spdlog::info(
                "Executor at client {} exited, status {}, time allocated {} us, polling {} us, execution {} us",
                i, std::get<1>(status), client.allocation_time,
                client.accounting.data()[i].hot_polling_time / 1000.0,
                client.accounting.data()[i].execution_time / 1000.0
              );
              client.executor.reset(nullptr);
              spdlog::info("Finished cleanup");
            }
          }
        }
      }
      if(removals.size()) {
        for(auto it : removals) {
          spdlog::info("Remove client id {}", it->first);
          _clients.erase(it);
        }
      }
    }
    spdlog::info("Background thread stops processing RDMA events.");
    _clients.clear();
  }

  //void Manager::poll_rdma()
  //{
  //  // FIXME: sleep when there are no clients
  //  bool active_clients = true;
  //  while(active_clients) {
  //    // FIXME: not safe? memory fance
  //    atomic_thread_fence(std::memory_order_acquire);
  //    for(int i = 0; i < _clients.size(); ++i) {
  //      Client & client = _clients[i];
  //      if(!client.active())
  //        continue;
  //      //auto wcs = client.connection->poll_wc(rdmalib::QueueType::RECV, false);
  //      auto wcs = client.rcv_buffer.poll(false);
  //      if(std::get<1>(wcs)) {
  //        SPDLOG_DEBUG(
  //          "Received at {}, work completions {}, clients active {}, clients datastructure size {}",
  //          i, std::get<1>(wcs), _clients_active, _clients.size()
  //        );
  //        for(int j = 0; j < std::get<1>(wcs); ++j) {
  //          auto wc = std::get<0>(wcs)[j];
  //          if(wc.status != 0)
  //            continue;
  //          uint64_t id = wc.wr_id;
  //          int16_t cores = client.allocation_requests.data()[id].cores;
  //          char * client_address = client.allocation_requests.data()[id].listen_address;
  //          int client_port = client.allocation_requests.data()[id].listen_port;

  //          if(cores > 0) {
  //            int secret = (i << 16) | (this->_secret & 0xFFFF);
  //            uint64_t addr = _accounting_data.address() + sizeof(Accounting)*i;
  //            // FIXME: Docker
  //            client.executor.reset(
  //              ProcessExecutor::spawn(
  //                client.allocation_requests.data()[id],
  //                _settings,
  //                {this->_address, this->_port, secret, addr, _accounting_data.rkey()}
  //              )
  //            );
  //            spdlog::info(
  //              "Client {} at {}:{} has executor with {} ID and {} cores",
  //              i, client_address, client_port, client.executor->id(), cores
  //            );
  //          } else {
  //            spdlog::info("Client {} disconnects", i);
  //            if(client.executor) {
  //              auto now = std::chrono::high_resolution_clock::now();
  //              client.allocation_time +=
  //                std::chrono::duration_cast<std::chrono::microseconds>(
  //                  now - client.executor->_allocation_finished
  //                ).count();
  //            }
  //            client.disable(i, _accounting_data.data()[i]);
  //            --_clients_active;
  //            break;
  //          }
  //        }
  //      }
  //      if(client.active()) {
  //        client.rcv_buffer.refill();
  //        if(client.executor) {
  //          auto status = client.executor->check();
  //          if(std::get<0>(status) != ActiveExecutor::Status::RUNNING) {
  //            auto now = std::chrono::high_resolution_clock::now();
  //            client.allocation_time +=
  //              std::chrono::duration_cast<std::chrono::microseconds>(
  //                now - client.executor->_allocation_finished
  //              ).count();
  //            // FIXME: update global manager
  //            spdlog::info(
  //              "Executor at client {} exited, status {}, time allocated {} us, polling {} us, execution {} us",
  //              i, std::get<1>(status), client.allocation_time,
  //              _accounting_data.data()[i].hot_polling_time,
  //              _accounting_data.data()[i].execution_time
  //            );
  //            client.executor.reset(nullptr);
  //          }
  //        }
  //      }
  //    }
  //  }
  //}

}
