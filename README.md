# Pair-With-Peer

The system consists of several clients (peers) and a central server. A peer can join the p2p network by connecting to the server. After entering the network, a peer has to register its username and then it can either share or download a file from other peers. The data being distributed are split into chunks. For each file, the server keeps track of the list of chunks each peer has. Any peer can download files from other peers directly. Moreover, any peer is capable of downloading different chunks of a file simultaneously from different peers.


Basics of the following concepts were used:-
● Multithreading in C++
● Basic Concepts of Network
● File Descriptor
● Socket Programming

The following is the step by step procedure of creating this project : 
  1> Creating sockets on both server and client side.
  2>Setting up sockets for further communication between server and client.
  3>Building of Handle_Client_Request function and Registering Client on the Server Side.
  4> Making the user interface of the client by which they can make requests to the server.
  5> Building up supporting functions for user interface.
  6> Functions used in User_Interface Function.
  7> Handling Requests from the Client.
  8> Creating functions to handle requests of clients. (On Client Side)
    ● searchfile(Sock_fd)
    ● sharefile_public(Sock_fd)
    ● receivefile(Sock_fd)
      ○ download(Sock_fd , filename)
    ● send_file_list(Sock_fd)
  9> Giving response of client request (On Server Side)
    ● Register_Client(Sock_fd)
    ● !Handle_Download_Request(Sock_fd)
    ● Handle_List_Request(Sock_fd)
    ● Provide_UserInfo(Sock_fd)
