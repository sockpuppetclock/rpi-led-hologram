#include <zmq.hpp>
#include <string>
#include <iostream>
#include <sstream>

int main()
{
   //  Prepare our context and socket
   zmq::context_t context (1);
   zmq::socket_t socket (context, zmq::socket_type::req);

   std::cout << "Connecting to hello world server..." << std::endl;
   socket.connect ("tcp://localhost:5555");

   //  Do 10 requests, waiting each time for a response
   for (int request_nbr = 0; request_nbr != 10; request_nbr++) {
       zmq::message_t request (5);
       memcpy (request.data (), "Hello", 5);
       std::cout << "Sending Hello " << request_nbr << "..." << std::endl;
       socket.send (request, zmq::send_flags::none);

       //  Get the reply.
       zmq::message_t reply;
       socket.recv (reply, zmq::recv_flags::none);
       std::cout << "Received World " << request_nbr << std::endl;
   }
   return 0;

   // zmq::context_t context (1);

   // //  Socket to talk to server
   // std::cout << "Collecting updates from weather server...\n" << std::endl;
   // zmq::socket_t subscriber (context, zmq::socket_type::sub);
   // subscriber.connect("tcp://localhost:5556");

   // //  Subscribe to zipcode, default is NYC, 10001
	// const char *filter = (argc > 1)? argv [1]: "10001 ";
   // subscriber.set(zmq::sockopt::subscribe, filter);

   // //  Process 100 updates
   // int update_nbr;
   // long total_temp = 0;
   // for (update_nbr = 0; update_nbr < 100; update_nbr++) {

   //    zmq::message_t update;
   //    int zipcode, temperature, relhumidity;

   //    subscriber.recv(update, zmq::recv_flags::none);

   //    std::istringstream iss(static_cast<char*>(update.data()));
   // iss >> zipcode >> temperature >> relhumidity;

   // total_temp += temperature;
   // }
   // std::cout 	<< "Average temperature for zipcode '"<< filter
   //       <<"' was "<<(int) (total_temp / update_nbr) <<"F"
   //       << std::endl;
   return 0;
}