/*
  Author:             Jason You
  Project:            HTTP Server
  Description:        Implementing the HTTP server with the socket functions in
                      the C/C++ programming language
  Last Modified Date: Nov. 19 2018
*/

/** Special Note for concurrency handling strategy:
  *
  *   This server only implemented the thread, process, and the
  *   pool of thread concurrency handling, but no process pool.
  */

/*---------------- C Packages ----------------*/
#include <dirent.h>       // for listing directory
#include <errno.h>
#include <limits.h>       // for PATH_MAX macro
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>			  // for sigaction()
#include <stdlib.h>
#include <string.h>     	// for strcmp() and strcasecmp()
#include <stdio.h>
#include <sys/stat.h>     // for struct stat
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>     	// for "int chroot(string path);"
// #include <pthread.h>		// not needed

/*---------------- C++ Packages ----------------*/
#include <algorithm>
#include <array>          // for exec helper function
#include <cerrno>         // for errno
#include <fstream>
#include <iostream>
#include <map>            // for MIME Map
#include <memory>
#include <mutex>          // std::mutex (since C++11)
#include <regex>          // for getting suffix of a filename
#include <stdexcept>
#include <string>
#include <sstream>
#include <thread>         // std::thread
#include <vector>         // for split()

/*---------------- Global Macros ----------------*/
#define DEFAULT_PORT 9999   // the default port number
#define POOL_LIMIT 5        // size of the process pool
#define SERVER_STRING "Server: CS 50011 lab7\r\n"

/*---------------- Global Variables ----------------*/

using namespace std;

int QueueLength = POOL_LIMIT;     // For a limited pool of processes
int Concurrency = 0;              // 0: default  1:-f  2:-p  3:-t 
int port = DEFAULT_PORT;          // Store the default port number
std::stringstream ss;			        // For string concatenation
std::mutex mtlock;			  	      // Mutex lock will only be released by the
								                  // thread who locked it.
std::string root = "";            // store the root directory  
std::string doc_req,              // client header info details
            method,
            http_version;
std::string sortBy = "name";      // determine to sort by "name", "size",
                                  // or "date" last modified.

// an incomplete map to refer to the MIME types
std::map<std::string, std::string> MIMEMap;

string usage = 
"\n"
"Usage myhttpd:\n"
"To use it in one window type:\n"
"   myhttp [-f|-t|-p] [<port>]\n"
"Where 1024 < port < 65536, and default [<port>] = 9999.\n"
"   -f: Creating a new process for each request\n"
"   -t: Creating a new thread for each request\n"
"   -p: Generating a pool of thread (not process)\n"
"In another window type:\n"
"   telnet <host> <port>\n"
"\n";

/*---------------- Helper Functions ----------------*/
bool endsWith(const char * str1, const char * str2);
void processRequestThread(int socket);
void processRequest(int);
void poolSlave(int masterSocket);
void sort_dir(char * cwd, char * docPath, DIR* dirp, int case1, int socket);
int acceptConnection(int masterSocket);

/*---------------- Client Response ----------------*/
void headers(int client, const char * filename);
void serve_file(int client, const char * filename);
void cat(int, FILE*);                   // put the file content on a socket
void not_found(int client);             // 404: page not found
void unimplemented(int client);         // 501: method unimplemented
void execute_cgi(int, const char *, const char *);
std::vector<char*> listDir(const char *);

/*---------------- Utility Functions ----------------*/
void enter_chroot(const char * root);
void resetStringStream();	  // send ss as a pointer
std::string myExec(const char*);					      // get the output from bash
void reapZombie(struct sigaction sa);
void sigchld_handler(int s);					   // for terminating process
void getRealpath(const char * filename, char * resolved);
void initMIMEMap();                      // -std=C++11 and up
void clearMap();
std::string getMIME(std::string filename);
std::string getMatch(const std::string filename, std::regex rgx);
void getHeaderInfo(std::string*);
// std::vector<std::string> split(std::string, char);
std::string listContent(std::vector<char*> v);
void generatePage(const char *);
void printCwd();

/*---------------- Main Program Start ----------------*/
/*----------------------------------------------------*/
int main( int argc, char **argv) {
  // Print usage if not enough arguments
  if ( argc > 3 ) {
  	cerr << "Error: got more than 2 arguments. Check the usage." << endl << usage;
    exit(-1);
  }

  // Get the type of concurrency handling strategy
  if ( argc == 3 ) {
    if ( strcmp(argv[1], "-f") == 0 ) {
      // got the -f
      Concurrency = 1;
    } else if ( strcmp(argv[1], "-t") == 0 ) {
      // got the -t
      Concurrency = 2;
    } else if ( strcmp(argv[1], "-p") == 0 ) {
      // got the -p
      Concurrency = 3;
    } else {
      // illegal argument
      cerr << "Error: illegal option. Check the usage." << endl << usage;
      exit(-1);
    }
  }

  // Get the valid port from the arguments
  if (argc == 2)
    // the second argument is the port number
    port = atoi(argv[1]);
  else if (argc == 3)
    // the third argument is the port number
    port = atoi(argv[2]);
  else
    // using the default port
    port = DEFAULT_PORT;

  if (port < 1024 || port > 65535) {
    // illegal port number
  	cerr << "Error: illegal port number. Check the usage." << endl << usage;
    exit(-1);
  } 

  // Set the IP address and port for this server
  struct sockaddr_in serverIPAddress;
  memset(&serverIPAddress, 0, sizeof(serverIPAddress));  // set all the serverIPAddress to be 0
  serverIPAddress.sin_family = AF_INET;  // prevent using privileged ports (less than 1024)
  serverIPAddress.sin_addr.s_addr = INADDR_ANY;  // the socket will be bound to all local interfaces
  serverIPAddress.sin_port = htons((u_short) port);  // using htons to convert host value to network byte order (big endian order)
  struct sigaction sa;					// for later reap zombie/dead processes

  // Allocate a socket (PF_INET refers to anything in the protocol, usually sockets/ports.)
  // SOCK_STREAM means that it is a TCP socket. SOCK_DGRAM means that it is a UDP socket.
  int masterSocket = socket(PF_INET, SOCK_STREAM, 0);
  if (masterSocket < 0) {
    perror("socket");
    exit(-1);
  }
  
  // Set socket options to reuse port. Otherwise we will
  // have to wait about 2 minutes before reusing the sae port number
  int optval = 1;
  int err = setsockopt(masterSocket, SOL_SOCKET, SO_REUSEADDR,
                      (char *)&optval, sizeof(int));
  if (err != 0) {
  	perror("setsockopt");
  	exit(-1);
  }

  // Bind the socket to the IP address and port
  // Here the "::" asked the compiler to look for bind() in the global namespace
  int error = ::bind(masterSocket,
      (struct sockaddr *)&serverIPAddress,
      sizeof(serverIPAddress));
  if (error) {
    perror("bind");
    exit(-1);
  }

  // Put socket in listening mode and set the
  // size of the queue of unprocessed connections
  error = listen(masterSocket, QueueLength);
  if (error) {
    perror("listen");
    exit(-1);
  }

  // initializing the MIME map [ usage : std::string getMIME(suffix) ]
  initMIMEMap();
  
  // Get host information with bash command and output
  const char * cmd = "cat /etc/hosts | egrep \"(purdue|localhost)\" | awk '{print $1,$2}'";
  std::string result = myExec(cmd);
  std::cout << "-------------The hosts------------- " << endl << result << endl;

  // Reap zombie/dead processes
  reapZombie(sa);

  // ---------------- Ready for incomming connections ----------------
  while (Concurrency != 3) {
    int slaveSocket = acceptConnection(masterSocket);

	// choosing the concurrency handling strategy
	switch (Concurrency) {
		case 0:		// Default no concurrency handling strategy
			processRequest(slaveSocket);
			close(slaveSocket);
			break;
		case 1:	{	// Process based
			pid_t slave = fork();
			if (slave == 0) {
				// This is a child process
				processRequest(slaveSocket);
				close(slaveSocket);
				exit(0);	// EXIT_SUCCESS
			}
			close(slaveSocket);
			break;
		} case 2: {		// Thread based
			std::thread th(processRequestThread, slaveSocket);
			th.join();
			close(slaveSocket);
			break;
		} default:	// should never reach here
			cerr << "Unknow concurrency handling strategy." << endl;
			exit(-1);
	}
  }

  // Using the thread-pool based concurrency handling strategy
  if (Concurrency == 3) {
  	for (int i = 0; i < QueueLength; i++) {
  		std::thread th(poolSlave, masterSocket);
  		th.join();		// is this the right approach?
  	}
  } else {
  	// Should never reach here
  	cerr << "Error: illegal concurrency handling strategy." << endl;
  	exit(-1);
  }

  return 0;
}

/*---------------- Helper Functions Start ----------------*/
/*--------------------------------------------------------*/

/**
  * @masterSocket: 	the master socket.
  * return:			the slave socket describer.
  * Accept incoming connections for non-thread-pool based strategy.
  */
int acceptConnection(int masterSocket) {
    struct sockaddr_in clientIPAddress;
    int alen = sizeof(clientIPAddress);
    int slaveSocket = accept(masterSocket,
          (struct sockaddr *)&clientIPAddress,
          (socklen_t*)&alen);

    // checking error code, and only report it with errno != EINTR
    if ( (slaveSocket < 0) && (errno =! EINTR)) {
        perror( "accept" );
	    exit( -1 );
	}
	return slaveSocket;
}

/**
  *	@root: 	the root directory to be used
  */
void enter_chroot(const char * root) {
	/* chroot */
	char resolved[PATH_MAX + 1];
	getRealpath(root, resolved);
	chdir(resolved);
  // commeted out since not works so well at this point
	if (chroot(resolved) != 0) {
		perror("chroot");
		exit(-1);
	}
}

/**
  * @filename: 	the filename used to find the real/absolute path in the system
  * return:		the real path for the given filename.
  */
void getRealpath(const char * filename, char * buf) {
	char * resolved = realpath(filename, buf);
	if (resolved == NULL) {
		perror("realpath");
		exit(EXIT_FAILURE);
	}
}

/*
 * used for debug: print current working directory
 */
void printCwd() {
  char cwd[PATH_MAX + 1];
    getcwd(cwd, sizeof(cwd));
    printf("Current working dir: %s\n", cwd);
}

/**
  *	@sa: the socket address structure
  * Reap all the zombie/dead processes
  */
void reapZombie(struct sigaction sa) {
	sa.sa_handler = sigchld_handler;    // reap all dead processes
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    // for reaping the zombie processes
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }
}

/**
  * Together with the reapZombie function to terminate zombie processes
  */
void sigchld_handler(int s) {
    int saved_errno = errno;     // waitpid() might overwrite errno, so we save and restore it
    while (waitpid(-1, NULL, WNOHANG) > 0);     // wait for a child process to stop or terminate
    errno = saved_errno;
}

/**
  * @&ss: the address of the stringstream that need to be reseted
  *
  * Reset the stringstream by: reset the underlying sequence to an emtpy
  * string with str and to clear any fail and eof flags with clear.
  */
void resetStringStream() {
  ss.str( std::string() );
	ss.clear();
}

/**
  * @cmd: 	the bash command to be executed
  * return:	the bash command console output
  */
std::string myExec(const char* cmd) {
    std::array<char, 128> buffer;
    std::string result;
    std::shared_ptr<FILE> pipe(popen(cmd, "r"), pclose);
    if (!pipe) throw std::runtime_error("popen() failed!");
    while (!feof(pipe.get())) {
        if (fgets(buffer.data(), 128, pipe.get()) != nullptr)
            result += buffer.data();
    }
    return result;
}

/**
  * @socket: the slave socket that waiting for connections from client
  * We need this function since the new thread won't return back, thus
  * we need to include the close() in the callable function.
  */
void processRequestThread(int socket) {
	processRequest(socket);
	close(socket);
}

/**
  * @socket: the master socket used to accept new connection in each thread
  * Building a new thread that can accept new incoming connections
  */
void poolSlave(int masterSocket) {
	while(1) {
		// ---------------- mutex lock ----------------
		// make sure that the shared resources is not used
		mtlock.lock();

		int slaveSocket = acceptConnection(masterSocket);

		mtlock.unlock();
		// ---------------- mutex unlock ----------------
		processRequest(slaveSocket);
		close(slaveSocket);
	}
}

/**
  *
  */
void processRequest(int fd) {
  if (Concurrency == 2 || Concurrency == 3)
  	mtlock.lock();   // lock for thread based or thread pool based

  // Buffer used to store the request received from the client
  const int MaxBuf = 2048;
  char buf[MaxBuf + 1];
  int n;              // byte read from fd
  int cgi = 0;        // 0: not a cgi program, or 1: is a cgi program
  struct stat st;     // for file checking
  std::string path;   // the path to the requested document
  std::string query_string;   // the query string for cgi
  const char * cwd = "./";    // current working directory (for page generation)


  //
  // The client should send <name><cr><lf>
  // Read the name of the client character by character until a
  // <CR><LF> is found.
  //
  if ((n = read(fd, &buf, MaxBuf)) < 0) {
    perror("read");
    exit(-1);
  }

  // Here we got the client header
  std::string cheader(buf);
  // Parse the header info and store them in the global variables
  getHeaderInfo(&cheader);
  /* we got the "method, http_version, doc_req" details */

  // ---------------- change root directory ---------------------------
  // Assuming "http-root-dir" is in the same directory of this server
  // ------------------------------------------------------------------
  if (root.length() == 0) {     // only run this once
    if (doc_req.find("icons/") != std::string::npos) {
      root = "http-root-dir/icons";
    } else {
      root = "http-root-dir/htdocs";
    }
    enter_chroot(root.c_str());    // not works so well, not bash compatible
  }

  generatePage(cwd);          // generate the index.html in cwd

  // Decide whether this is a CGI Program
  if (strcasecmp(method.c_str(), "POST") == 0)
    cgi = 1;
  else if (strcasecmp(method.c_str(), "GET") == 0) {
    std::size_t pos = doc_req.find("?");
    if (pos != std::string::npos) {
      cgi = 1;
      // parse query string
      query_string = doc_req.substr(pos + 1);
    }
  } else {
    // method not implemented
    unimplemented(fd);
    return;
  }

  // building the path
  resetStringStream(); // ss need to be passed as a pointer
  ss << doc_req;

  // request ends with forward slash '/', then append index.html
  if (doc_req.back() == '/') {
    ss << "index.html";
  }
  path = ss.str();

  if (stat(path.c_str(), &st) == -1) {
    not_found(fd);
  } else {
    // S_IFMT: the bit mask for the file type field
    if ((st.st_mode & S_IFMT) == S_IFDIR) {
      // if the requested document is a dir, generate an index.html for it
      // [must first generate the page before find get path to index.html]

      // must change directory in here [or add the path, e.g. /dir1/...]
      const char * subdir = path.c_str();
      chdir(subdir);      // here path.c_str() is the subdir
      generatePage(cwd);

      // first generate the html page, but with the parent dir name appended

      // get the real path for index.html
      resetStringStream();
      ss << "index.html";
      path = ss.str();
    }

    // if ((st.st_mode & S_IXUSR) || (st.st_mode & S_IXGRP) ||
    //     (st.st_mode & S_IXOTH))
    //   cgi = 1;   // not implemented so comment out

    if (!cgi) {
      serve_file(fd, path.c_str());
    }
    else
      execute_cgi(fd, path.c_str(), query_string.c_str());
  }

  if (Concurrency == 2 || Concurrency == 3)
  	mtlock.unlock();
}

/*---------------------- Map for MIME ------------------------------*/
/*------------------------------------------------------------------*/

/**
  * Initializing the MIME map as a global variable, compiler -std=c++11 or up
  * 
  * application/postscript         ai eps ps       
  * application/rtf                rtf             
  * application/slate                              
  * application/x-tex              tex             
  * application/x-texinfo          texinfo texi   
  * application/x-troff            t tr roff       
  * audio/basic                    au snd          
  * audio/x-aiff                   aif aiff aifc
  * audio/x-wav                    wav             
  * image/gif                      gif             
  * image/ief                      ief             
  * image/jpeg                     jpeg jpg jpe
  * image/tiff                     tiff tif        
  * image/x-xwindowdump            xwd             
  * text/html                      html            
  * text/plain                     txt             
  * video/mpeg                     mpeg mpg mpe    
  * video/quicktime                qt mov          
  * video/x-msvideo                avi             
  * video/x-sgi-movie              movie  
*/
void initMIMEMap() {
  MIMEMap = {
    {"ai", "application/postscript"}, {"eps", "application/postscript"},
    {"ps", "application/postscript"}, {"rtf", "application/rtf"},
    {"tex", "application/x-tex"}, {"texinfo", "application/x-texinfo"},
    {"texi", "application/x-texifo"}, {"t", "application/troff"},   
    {"tr", "application/x-troff"}, {"roff", "application/x-troff"},
    {"au", "audio/basic"}, {"snd", "audio/basic"},
    {"aif", "audio/x-aiff"}, {"aiff", "audio/x-aiff"}, 
    {"aifc", "audio/x-aiff"}, {"wav", "audio/x-wav"}, 
    {"gif", "image/gif"}, {"ief", "image/ief"}, 
    {"jpeg", "image/jpeg"}, {"jpg", "image/jpeg"}, 
    {"jpe", "image/jpeg"}, {"tiff", "image/tiff"},  
    {"tif", "image/tiff"}, {"xwd", "image/x-xwindowdump"}, 
    {"html", "text/html"}, {"txt", "text/plain"},
    {"mpeg", "video/mpeg"}, {"mpg", "video/mpeg"},
    {"mpe", "video/mpeg"}, {"qt", "video/quicktime"},
    {"mov", "video/quicktim"}, {"avi", "video/x-msvideo"},
    {"movie", "video/x-sgi-movie"}
  };
}

/**
  * Clear the map if needed.
  */
void clearMap() {
  MIMEMap.clear();
}

/**
  * @filename:  the file name with suffix in it
  * return:   the matched string of the given file name
  * or "" (empty string) if no match found.
  * [ remember to use parenthesis to mark the desired part ]
  */
std::string getMatch(const std::string filename, std::regex rgx) {
  std::smatch match;
  
  if (std::regex_search(filename.begin(), filename.end(), match, rgx))
    return match[1];
  else
    return "";    // empty string
}

/**
  * @suffix:  the suffix used to match the proper MIME type;
  * return:   the proper MIME type if found, other wise, return text/plain
  */
std::string getMIME(std::string filename) {

  std::regex rgx("^.*\\.(\\w+)$");  // for extracting the suffix
  std::string suffix = getMatch(filename, rgx);

  if (suffix.length() == 0)
    return "text/plain";

  std::map<std::string, std::string>::iterator it;

  if ( (it = MIMEMap.find(suffix)) != (MIMEMap.end()) ) {
    // found the suffix and return the MIME type
    return it->second;
  } else {
    // return the default text type
    return "text/plain";
  }
}

/*---------------------- Extract Header Info -----------------------*/
/*------------------------------------------------------------------*/

/**
* @cheader: the client header string pointer
* Set the above string pointers with proper values extracted
* from the client header string pointer.
*/
void getHeaderInfo(std::string *cheader) {
  // document requested
  std::regex doc_req_rgx("^GET (.+) HTTP");
  // http request method
  std::regex method_rgx("^(\\w+) .+ HTTP");
  // http version
  std::regex http_version_rgx("^GET .+ (HTTP/1.[10])");
  
  // get all the requested parts
  doc_req = getMatch(*cheader, doc_req_rgx);
  method = getMatch(*cheader, method_rgx);
  http_version = getMatch(*cheader, http_version_rgx);
}

/*---------------- Client Response --------------------------------*/
/*-----------------------------------------------------------------*/

/**
  * @client:    the socket connects to the client
  * @filename:  the name of the file to be served
  * Send a regular file to the client. Report error if any.
  */
void serve_file(int client, const char * filename) {
  FILE * file = NULL;

  file = fopen(filename, "r");
  if (file == NULL) {
    cout << "NOT_FOUND in serve_file |" << filename << "|" << endl;
    not_found(client);
  }
  else {
    headers(client, filename);
    cat(client, file);
  }
  fclose(file);
}

/**
  * @ client: the client socket
  * @ path: the path to the CGI program
  * @ query_string: the query string
  * Execute a CGI script.
  */
void execute_cgi(int client, const char *path, const char *query_string) {

}

/**
  * @client:  the socket connects to the client
  * @file:    FILE pointer for the file to cat
  * Put the entire contents of a file on to the client socket
  */
void cat(int client, FILE * file) {
  char buf[1024];
  fgets(buf, sizeof(buf), file);
  while (!feof(file)) {
    write(client, buf, strlen(buf) + 1);
    fgets(buf, sizeof(buf), file);
  }
}

/**
  * @client: the socket connects to the client
  * @filename: the name of the file
  * Send HTTP headers to the client
  */
void headers(int client, const char * filename) {
  // use file name to determine file type
  std::string fn(filename);
  std::string content_type = getMIME(fn);

  cout << "\t\tfilename = " << fn << endl
      << "\t\tcontent_type = " << content_type << endl;

  // Building the page
  resetStringStream(); // ss need to be passed as a pointer
  ss << http_version << " 200 OK\r\n"
     << "Server: " << SERVER_STRING
     << "Content-Type: " << content_type << "\r\n"
     << "\r\n";
  std::string page = ss.str();
  write(client, page.c_str(), page.length() + 1);
}

/**
  * @client: the socket connects to the client
  * Sends a 404 page to the client.
  */
void not_found(int client) {
  // Building the page
  resetStringStream(); // ss need to be passed as a pointer
  ss << http_version << " 404 Not Found\r\n"
     << "Server: " << SERVER_STRING
     << "Content-Type: text/html\r\n"
     << "\r\n"
     << "<html><title>404: Page Not Found</title><body>\n"
     << "<h1>404: Page Not Found</h1>\n"
     << "</body></html>\n";
  std::string page = ss.str();
  // send the page to the client
  write(client, page.c_str(), page.length() + 1);
}

/**
  * @client: the socket connects to the client
  * Sends a 501 page to the client.
  */
void unimplemented(int client) {
  // Building the page
  resetStringStream(); // ss need to be passed as a pointer
  ss << http_version << " 501 Method Not Implemented\r\n"
     << "Server: " << SERVER_STRING
     << "Content-Type: text/html\r\n"
     << "\r\n"
     << "<html><title>501: Method Not Implemented</title><body>\n"
     << "<h1>501: Method Not Implemented</h1>\n"
     << "</body></html>\n";
  std::string page = ss.str();
  // send the page to the client
  write(client, page.c_str(), page.length() + 1);
}
/**
* @path: the path that need to be listed
* Store a directory/file list to the string stream
*/
std::vector<char*> listDir(const char * path) {
  std::vector<char*> result;  // store the file/dir name result
  
  DIR *dir;
  struct dirent *ent;
  if ((dir = opendir (path)) != NULL) {
    /* print all the files and directories within directory */
    while ((ent = readdir (dir)) != NULL) {
      if (ent->d_name[0] != '.' || ent->d_name[1] != '\0') {
        // ignore the dot '.' directory
        result.push_back(ent->d_name);
      }
    }
    closedir (dir);
  } else {
    /* could not open directory */
    perror ("listDir");
    exit(-1);
  }
  sort(result.begin(), result.end());
  return result;
}

/**
  * @v: the string vector contain all the dir/file names
  * Return a string that contains the html dir/file list
  */
std::string listContent(std::vector<char*> v) {
  resetStringStream();
  
  char path[PATH_MAX + 1];
  // clear the array
  memset(path, 0, sizeof path);
  for (int i = 0; i < v.size(); i++) {
    getRealpath(v[i], path);
    ss << "    <li><A HREF=\"" << path << "\">"
        << v[i] << "</A>\n";
  }
  return ss.str();
}

/*
 * @path: the path want to be generated as index.html
 * generate a list of file/dir in html format
 */
void generatePage(const char * path) {
  // parse the output into an array
  vector<char*> result;
  result = listDir(path);
  std::ofstream fout;
  fout.open("index.html");
  std::string page_head = 
    "<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML//EN\">\n"
    "<html>\n"
    "  <head>\n"
    "    <title>CS50011: HTTP Server</title>\n"
    "    <link rel=\"icon\" type=\"image/x-icon\" href=\"favicon.ico\" />\n"
    "  </head>\n"
    "\n"
    "  <body>\n"
    "    <h1>CS50011: HTTP Server</h1>\n"
    "\n"
    "    <ul>\n";
    
  std::string page_mid = listContent(result);

  std::string page_tail =
    "    </ul>\n"
    "\n"
    "    <hr>\n"
    "    <address><a href=\"mailto:you18@purdue.edu\">Shengwei You</a></address>\n"
    "<!-- Created: Fri Dec  5 13:12:48 EST 1997 -->\n"
    "<!-- hhmts start -->\n"
    "Last modified: Mon Nov 19 2018\n"
    "<!-- hhmts end -->\n"
    "  </body>\n"
    "</html>\n"
    "";
  fout << page_head << page_mid << page_tail;
  fout.close();
}

