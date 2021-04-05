
#include <chrono>
#include <thread>

#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include <rdmalib/connection.hpp>
#include <rdmalib/allocation.hpp>

#include "manager.hpp"
#include "rdmalib/rdmalib.hpp"

namespace executor {

  Client::Client(std::unique_ptr<rdmalib::Connection> conn, ibv_pd* pd, Accounting & _acc):
    connection(std::move(conn)),
    allocation_requests(RECV_BUF_SIZE),
    rcv_buffer(RECV_BUF_SIZE),
    accounting(_acc),
    allocation_time(0)
  {
    // Make the buffer accessible to clients
    allocation_requests.register_memory(pd, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    // Initialize batch receive WCs
    connection->initialize_batched_recv(allocation_requests, sizeof(rdmalib::AllocationRequest));
    rcv_buffer.connect(connection.get());
  }

  //void Client::reinitialize(rdmalib::Connection* conn)
  //{
  //  connection = conn;
  //  // Initialize batch receive WCs
  //  connection->initialize_batched_recv(allocation_requests, sizeof(rdmalib::AllocationRequest));
  //  rcv_buffer.connect(conn);
  //}

  void Client::reload_queue()
  {
    rcv_buffer.refill();
  }

  void Client::disable()
  {
    rdma_disconnect(connection->_id);
    // SEGFAULT?
    //ibv_dereg_mr(allocation_requests._mr);
    connection->close();
    connection = nullptr;
  }

  bool Client::active()
  {
    // Compiler complains for some reason
    return connection.operator bool();
  }
  
  ActiveExecutor::~ActiveExecutor() {}

  ProcessExecutor::ProcessExecutor(ProcessExecutor::time_t alloc_begin, pid_t pid):
    _pid(pid)
  {
    _allocation_begin = alloc_begin;
    // FIXME: remove after connection
    _allocation_finished = _allocation_begin;
  }

  std::tuple<ProcessExecutor::Status,int> ProcessExecutor::check() const
  {
    int status;
    pid_t return_pid = waitpid(_pid, &status, WNOHANG);
    if(!return_pid) {
      return std::make_tuple(Status::RUNNING, 0);
    } else {
      if(WIFEXITED(status)) {
        return std::make_tuple(Status::FINISHED, WEXITSTATUS(status));
      } else if (WIFSIGNALED(status)) {
        return std::make_tuple(Status::FINISHED_FAIL, WTERMSIG(status));
      } else {
        // Unknown problem
        return std::make_tuple(Status::FINISHED_FAIL, -1);
      }
    }
  }

  int ProcessExecutor::id() const
  {
    return static_cast<int>(_pid);
  }

  ProcessExecutor* ProcessExecutor::spawn(
    const rdmalib::AllocationRequest & request,
    const ExecutorSettings & exec,
    const executor::ManagerConnection & conn
  )
  {
    // FIXME: doesn't work every well?
    //int rc = ibv_fork_init();
    int rc = 0;
    if(rc)
      exit(rc);

    auto begin = std::chrono::high_resolution_clock::now();
    int mypid = fork();
    if(mypid < 0) {
      spdlog::error("Fork failed! {}", mypid);
    }
    if(mypid == 0) {
      mypid = getpid();
      spdlog::info("Child fork begins work on PID {}", mypid);
      auto out_file = ("executor_" + std::to_string(mypid));
      // FIXME: number of input buffers
      std::string client_port = std::to_string(request.listen_port);
      std::string client_in_size = std::to_string(request.input_buf_size);
      std::string client_func_size = std::to_string(request.func_buf_size);
      std::string client_cores = std::to_string(request.cores);
      std::string client_timeout = std::to_string(request.hot_timeout);
      std::string executor_repetitions = std::to_string(exec.repetitions);
      std::string executor_warmups = std::to_string(exec.warmup_iters);
      std::string executor_recv_buf = std::to_string(exec.recv_buffer_size);
      std::string executor_max_inline = std::to_string(exec.max_inline_data);

      std::string mgr_port = std::to_string(conn.port);
      std::string mgr_secret = std::to_string(conn.secret);
      std::string mgr_buf_addr = std::to_string(conn.r_addr);
      std::string mgr_buf_rkey = std::to_string(conn.r_key);

      int fd = open(out_file.c_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
      dup2(fd, 1);
      dup2(fd, 2);
      const char * argv[] = {
        "bin/executor",
        "-a", request.listen_address,
        "-p", client_port.c_str(),
        "--polling-mgr", "thread",
        "-r", executor_repetitions.c_str(),
        "-x", executor_recv_buf.c_str(),
        "-s", client_in_size.c_str(),
        "--fast", client_cores.c_str(),
        "--warmup-iters", executor_warmups.c_str(),
        "--max-inline-data", executor_max_inline.c_str(),
        "--func-size", client_func_size.c_str(),
        "--timeout", client_timeout.c_str(),
        "--mgr-address", conn.addr.c_str(),
        "--mgr-port", mgr_port.c_str(),
        "--mgr-secret", mgr_secret.c_str(),
        "--mgr-buf-addr", mgr_buf_addr.c_str(),
        "--mgr-buf-rkey", mgr_buf_rkey.c_str(),
        nullptr
      };
      int ret = execve(argv[0], const_cast<char**>(&argv[0]), nullptr);
      spdlog::info("Child fork stopped work on PID {}", mypid);
      if(ret == -1) {
        spdlog::error("Executor process failed {}, reason {}", errno, strerror(errno));
        close(fd);
        exit(1);
      }
      close(fd);
      exit(0);
    }
    return new ProcessExecutor{begin, mypid};
  }

  Manager::Manager(std::string addr, int port, std::string server_file, const ExecutorSettings & settings):
    _clients_active(0),
    _state(addr, port, 32, true),
    _status(addr, port),
    _settings(settings),
    _accounting_data(MAX_CLIENTS_ACTIVE),
    _address(addr),
    _port(port),
    // FIXME: randomly generated
    _secret(0x1234)
  {
    _clients.reserve(this->MAX_CLIENTS_ACTIVE);
    std::ofstream out(server_file);
    _status.serialize(out);

    memset(_accounting_data.data(), 0, _accounting_data.data_size());
    _accounting_data.register_memory(_state.pd(), IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC);
  }

  void Manager::start()
  {
    spdlog::info("Begin listening and processing events!");
    std::thread listener(&Manager::listen, this);
    std::thread rdma_poller(&Manager::poll_rdma, this);

    listener.join();
    rdma_poller.join(); 
  }

  void Manager::listen()
  {
    while(true) {
      // Connection initialization:
      // (1) Initialize receive WCs with the allocation request buffer
      auto conn = _state.poll_events(
        false
      );
      if(conn->_private_data) {
        if((conn->_private_data & 0xFFFF ) == this->_secret) {
          int client = conn->_private_data >> 16;
          spdlog::info("Connected executor for client {}", client);
          _state.accept(conn);
          _clients[client].executor->connection = std::move(conn);
        } else {
          spdlog::error("New connection's private data that we can't understand: {}", conn->_private_data);
        }
      }
      // FIXME: users sending their ID 
      else {
        _clients.emplace_back(std::move(conn), _state.pd(), _accounting_data.data()[_clients_active]);
        _state.accept(_clients.back().connection);
        spdlog::info("Connected new client id {}", _clients_active);
        atomic_thread_fence(std::memory_order_release);
        _clients_active++;
      }
    }
  }

  void Manager::poll_rdma()
  {
    // FIXME: sleep when there are no clients
    bool active_clients = true;
    while(active_clients) {
      // FIXME: not safe? memory fance
      atomic_thread_fence(std::memory_order_acquire);
      for(int i = 0; i < _clients.size(); ++i) {
        Client & client = _clients[i];
        if(!client.active())
          continue;
        auto wcs = client.connection->poll_wc(rdmalib::QueueType::RECV, false);
        if(std::get<1>(wcs)) {
          SPDLOG_DEBUG(
            "Received at {}, work completions {}, clients active {}, clients datastructure size {}",
            i, std::get<1>(wcs), _clients_active, _clients.size()
          );
          for(int j = 0; j < std::get<1>(wcs); ++j) {
            auto wc = std::get<0>(wcs)[j];
            if(wc.status != 0)
              continue;
            uint64_t id = wc.wr_id;
            int16_t cores = client.allocation_requests.data()[id].cores;
            char * client_address = client.allocation_requests.data()[id].listen_address;
            int client_port = client.allocation_requests.data()[id].listen_port;

            if(cores > 0) {
              int secret = (i << 16) | (this->_secret & 0xFFFF);
              // FIXME: Docker
              client.executor.reset(
                ProcessExecutor::spawn(
                  client.allocation_requests.data()[id],
                  _settings,
                  {this->_address, this->_port, secret, 0, 0}
                )
              );
              spdlog::info(
                "Client {} at {}:{} has executor with {} ID and {} cores",
                i, client_address, client_port, client.executor->id(), cores
              );
            } else {
              spdlog::info("Client {} disconnects", i);
              client.disable();
              --_clients_active;
              break;
            }
          }
        }
        if(client.active()) {
          client.rcv_buffer.refill();
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
                "Executor at client {} exited, status {}, time allocated {} us",
                i, std::get<1>(status), client.allocation_time
              );
              client.executor.reset(nullptr);
            }
          }
        }
      }
    }
  }

}
