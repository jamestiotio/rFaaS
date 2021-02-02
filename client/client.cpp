
#include <cxxopts.hpp>


int main(int argc, char ** argv)
{

  cxxopts::Options options("serverless-rdma-client", "Invoke functions");
  options.add_options()
    ("d,debug", "Enable debugging", cxxopts::value<bool>()->default_value("false"))
    ("a,address", "Server address", cxxopts::value<std::string>())
    ("p,port", "Server port", cxxopts::value<int>())
    ("i,invocations", "Invocations", cxxopts::value<int>())
    ("n,name", "Function name", cxxopts::value<std::string>())
    ("v,verbose", "Verbose output", cxxopts::value<bool>()->default_value("false"))
  ;
  auto result = options.parse(argc, argv);

  // Start RDMA connection

  // Start measurement

  // Write request

  // Wait for reading results

  // Stop measurement

  return 0;
}
