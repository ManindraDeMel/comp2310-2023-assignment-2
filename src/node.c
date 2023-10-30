#include "csapp/csapp.h"
#include "utils.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

// You may assume that all requests sent to a node are less than this length
#define REQUESTLINELEN 128
#define HOSTNAME "localhost"

// Cache related constants
#define MAX_OBJECT_SIZE 512 // object here refers to the posting list or result list being cached
#define MAX_CACHE_SIZE MAX_OBJECT_SIZE*128
#define PARTITION_SIZE 100

// Assumed data structures
typedef struct {
    char key[REQUESTLINELEN];
    char value[MAX_OBJECT_SIZE];
} KeyValuePair;

// A dynamic list to manage cache. You can use an array but a linked list would allow 
// more flexibility in cache management, especially for LRU eviction.
typedef struct CacheItem {
    KeyValuePair data;
    struct CacheItem* next;
} CacheItem;

/* This struct contains all information needed for each node */
typedef struct node_info {
  int node_id;     // node number
  int port_number; // port number
  int listen_fd;   // file descriptor of socket the node is using
} node_info;

typedef enum {
    LIVE, 
    DEAD
} node_status;

typedef struct node_info_extended {
    node_info base_info; // Basic node information
    // Key-value store for the partition

    // Cache for storing recent queries and results

    node_status status; // Status of the node (LIVE/DEAD)
} node_info_extended;

/* Variables that all nodes will share */

// Port number of the parent process. After each node has been spawned, it 
// attempts to connect to this port to send the parent the request for it's own 
// section of the database.
int PARENT_PORT = 0;

// Number of nodes that were created. Must be between 1 and 8 (inclusive).
int TOTAL_NODES = 0;

// A dynamically allocated array of TOTAL_NODES node_info structs.
// The parent process creates this and populates it's values so when it creates
// the nodes, they each know what port number the others are using.
node_info_extended *NODES = NULL;

/* ------------  Variables specific to each child process / node ------------ */

CacheItem* cache_head = NULL;
int current_cache_size = 0;

// After forking each node (one child process) changes the value of this variable
// to their own node id.
// Note that node ids start at 0. If you're just implementing the single node 
// server this will be set to 0.
int NODE_ID = -1;

// Each node will fill this struct in with it's own portion of the database.
database partition = {NULL, 0, NULL};

/** @brief Called by a child process (node) when it wants to request its partition
 *         of the database from the parent process. This will be called ONCE by 
 *         each node in the "digest" phase.
 *  
 *  @todo  Implement this function. This function will need to:
 *         - Connect to the parent process. HINT: the port number to use is 
 *           stored as an int in PARENT_PORT.
 *         - Send a request line to the parent. The request needs to be a string
 *           of the form "<nodeid>\n" (the ID of the node followed by a newline) 
 *         - Read the response of the parent process. The response will start 
 *           with the size of the partition followed by a newline. After the 
 *           newline character, the next size bytes of the response will be this
 *           node's partition of the database.
 *         - Set the global partition variable. 
 */
void request_partition(void) {
    int client_fd;
    char request[REQUESTLINELEN];
    char responseline[REQUESTLINELEN];
    rio_t rio;
    Rio_readinitb(&rio, client_fd);

    // Connect to the parent process
    char port_str[6]; // Max 5 digits for a port and the null terminator
    sprintf(port_str, "%d", PARENT_PORT);
    client_fd = Open_clientfd(HOSTNAME, port_str);
    
    // Send a request line to the parent with the node's ID
    sprintf(request, "%d\n", NODE_ID);
    Rio_writen(client_fd, request, strlen(request));

    // Read the response from the parent
    Rio_readlineb(&rio, responseline, REQUESTLINELEN);
    int size = atoi(responseline);
    
    // Allocate memory for partition data and read it from the socket
    partition.m_ptr = (char *) Malloc(size);
    Rio_readnb(&rio, partition.m_ptr, size);


    // Close the client file descriptor
    Close(client_fd);
}
// 1. Check if key exists in partition
bool key_exists_in_partition(char* key, int node_id) {
    int index = find_entry(&partition, key);
    return (index != -1);
}
// 2. Check if key exists in cache
bool key_exists_in_cache(char* key, int node_id) {
    CacheItem* current = cache_head;
    while (current != NULL) {
        if (strcmp(current->data.key, key) == 0) {
            return true;
        }
        current = current->next;
    }
    return false;
}

/**
 * @brief Determines the target node for the given request.
 * 
 * @param request The request whose target node needs to be determined.
 * @return The ID of the target node.
 */
int determine_target_node(char* request) {
    unsigned int hash_value = 0;
    char *ptr = request;
    
    // Simple hash calculation: sum of ASCII values of characters in the request.
    while (*ptr) {
        hash_value += *ptr;
        ptr++;
    }

    // Modulus with the total number of nodes to get the target node ID.
    return hash_value % TOTAL_NODES;
}


// Function to remove the oldest item from the cache (LRU eviction)
void remove_oldest_from_cache() {
    CacheItem* to_remove = cache_head;
    cache_head = cache_head->next;
    free(to_remove);
    current_cache_size -= MAX_OBJECT_SIZE; // Assume each cache item uses the full object size
}

// Add an item to cache
void add_to_cache(char* key, char* value) {
    if (current_cache_size + MAX_OBJECT_SIZE > MAX_CACHE_SIZE) {
        remove_oldest_from_cache();
    }

    CacheItem* new_item = malloc(sizeof(CacheItem));
    strncpy(new_item->data.key, key, REQUESTLINELEN);
    strncpy(new_item->data.value, value, MAX_OBJECT_SIZE);
    new_item->next = cache_head;
    cache_head = new_item;

    current_cache_size += MAX_OBJECT_SIZE;
}

// 3. Forward request to another node
void forward_request_to_node(char* request, int target_node_id) {
    // Use NODES[target_node_id] to get the node's metadata
    node_info_extended target_node = NODES[target_node_id];
    
    struct sockaddr_in serveraddr;
    int clientfd;
    rio_t rio;

    if ((clientfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket error");
        exit(1);
    }

    bzero((char*) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    inet_pton(AF_INET, HOSTNAME, &(serveraddr.sin_addr));
    serveraddr.sin_port = htons(target_node.base_info.port_number);

    if (connect(clientfd, (struct sockaddr*) &serveraddr, sizeof(serveraddr)) < 0) {
        perror("Connect error");
        exit(1);
    }

    Rio_readinitb(&rio, clientfd);
    Rio_writen(clientfd, request, strlen(request));
    
    // Not handling response here. You may want to read the response 
    // and either cache it, or forward it back to the original client.
    close(clientfd);
}
void handle_request(char* request, int client_fd, int node_id) {
    if (key_exists_in_partition(request, node_id)) {
        // Fetch from partition and respond
    } else if (key_exists_in_cache(request, node_id)) {
        // Fetch from cache and respond
    } else {
        // Forward to another node or return a graceful failure message
        int target_node_id = determine_target_node(request); 
        if (NODES[target_node_id].status == LIVE) {
            forward_request_to_node(request, target_node_id);
        } else {
            char failure_msg[] = "Key not found in the system.";
            Rio_writen(client_fd, failure_msg, strlen(failure_msg));
        }
    }
}
/** @brief The main server loop for a node. This will be called by a node after
 *         it has finished the digest phase. The server will run indefinitely,
 *         responding to requests. Each request is a single line. 
 *
 *  @note  The parent process creates the listening socket that the node should
 *         use to accept incoming connections. This file descriptor is stored in
 *         NODES[NODE_ID].listen_fd. 
*/
void node_serve(void) {
    int client_fd;
    char request[REQUESTLINELEN];
    rio_t rio;

    while (1) {
        client_fd = Accept(NODES[NODE_ID].base_info.listen_fd, NULL, NULL);
        Rio_readinitb(&rio, client_fd);

        if (Rio_readlineb(&rio, request, REQUESTLINELEN) != 0) {
            handle_request(request, client_fd, NODE_ID);
        }

        Close(client_fd);
    }
}


/** @brief Called after a child process is forked. Initialises all information
 *         needed by an individual node. It then calls request_partition to get
 *         the database partition that belongs to this node (the digest phase). 
 *         It then calls node_serve to begin responding to requests (the serve
 *         phase). Since the server is designed to run forever (unless forcibly 
 *         terminated) this function should not return.
 * 
 *  @param node_id Value between 0 and TOTAL_NODES-1 that represents which node
 *         number this is. The global NODE_ID variable will be set to this value
 */
void start_node(int node_id) {
  NODE_ID = node_id;

  // close all listen_fds except the one that this node should use.
  for (int n = 0; n < TOTAL_NODES; n++) {
    if (n != NODE_ID)
      Close(NODES[n].base_info.listen_fd);
  }

  request_partition();
  node_serve();
}



/** ----------------------- PARENT PROCESS FUNCTIONS ----------------------- **/

/* The functions below here are for the initial parent process to use (spawning
 * child processes, partitioning the database, etc). 
 * You do not need to modify this code.
*/


/** @brief  Tries to create a listening socket on the port that start_port 
 *          points to. If it cannot use that port, it will subsequently try
 *          increasing port numbers until it successfully creates a listening 
 *          socket, or it has run out of valid ports. The value at start_port is
 *          set to the port_number the listening socket was opened on. The file
 *          descriptor of the listening socket is returned.
 * 
 *  @param  start_port The value that start_port points to is used as the first 
 *          port to try. When the function returns, the value is updated to the
 *          port number that the listening socket can use. 
 *  @return The file descriptor of the listening socket that was created, or -1
 *          if no listening socket has been created.
*/
int get_listenfd(int *start_port) {
  char portstr[PORT_STRLEN]; 
  int port, connfd;
  for (port = *start_port; port < MAX_PORTNUM; port++) {
    port_number_to_str(port, portstr);
    connfd = open_listenfd(portstr);
    if (connfd != -1) { // found a port to use
      *start_port = port;
      return connfd;
    }
  }
  return -1;
}

/** @brief  Called by the parent to handle a single request from a node for its
 *          partition of the database. 
 *
 *  @param  db The database that will be partitioned. 
 *  @param  connfd The connected file descriptor to read the request (a node id) 
 *          from. The partition of the database is written back in response.
 *  @return If there is an error in the request returns -1. Otherwise returns 0.
*/
int parent_handle_request(database *db, int connfd) {
  char request[REQUESTLINELEN];
  char responseline[REQUESTLINELEN];
  char *response;
  int node_id;
  ssize_t rl;
  size_t partition_size = 0;
  if ((rl = read(connfd, request, REQUESTLINELEN)) < 0) {
    fprintf(stderr, "parent_handle_request read error: %s\n", strerror(errno));
    return -1;
  }
  sscanf(request, "%d", &node_id);
  if ((node_id < 0) || (node_id >= TOTAL_NODES)) {
    response = "Invalid Request.\n";
    partition_size = strlen(response);
  } else {
    response = get_partition(db, TOTAL_NODES, node_id, &partition_size);
  }
  snprintf(responseline, REQUESTLINELEN, "%lu\n", partition_size);
  rl = write(connfd, responseline, strlen(responseline));
  rl = write(connfd, response, partition_size);
  return 0;
}

/** Called by the parent process to load in the database, and wait for the child
 *  nodes it created to send a message requesting their portion of the database.
 *  After it has received the same number of requests as nodes, it unmaps the 
 *  database. 
 *
 *  @param db_path path to the database file being loaded in. It is assumed that
 *         the entries contained in this file are already sorted in alphabetical
 *         order.
 */
void parent_serve(char *db_path, int parent_connfd) {
  // The parent doesn't need to create/populate the hash table.
  database *db = load_database(db_path);
  struct sockaddr_storage clientaddr;
  socklen_t clientlen = sizeof(clientaddr);
  int connfd = 0;
  int requests = 0;

  while (requests < TOTAL_NODES) {
    connfd = accept(parent_connfd, (SA *)&clientaddr, &clientlen);
    parent_handle_request(db, connfd);
    Close(connfd);
    requests++;
  }
  // Parent has now finished it's job.
  Munmap(db->m_ptr, db->db_size);
}

/** @brief Called after the parent has finished sending each node its partition 
 *         of the database. The parent waits in a loop for any child processes 
 *         (nodes) to terminate, and prints to stderr information about why the 
 *         child process terminated. 
*/
void parent_end() {
  int stat_loc;
  pid_t pid;
  while (1) {
    pid = wait(&stat_loc);
    if (pid < 0 && (errno == ECHILD))
      break;
    else {
      if (WIFEXITED(stat_loc))
        fprintf(stderr, "Process %d terminated with exit status %d\n", pid, WEXITSTATUS(stat_loc));
      else if (WIFSIGNALED(stat_loc))
        fprintf(stderr, "Process %d terminated by signal %d\n", pid, WTERMSIG(stat_loc));
    }
  }
}

int main(int argc, char const *argv[]) {
  int start_port;    // port to begin search
  int parent_connfd; // parent listens here to handle distributing database 
  int n_connfd;      
  pid_t pid;
  
  if (argc != 4) {
    fprintf(stderr, "usage: %s [num_nodes] [starting_port] [name_of_file]\n", argv[0]);
    exit(1);
  }
  
  sscanf(argv[1], "%d", &TOTAL_NODES);
  sscanf(argv[2], "%d", &start_port);

  if (TOTAL_NODES < 1 || (TOTAL_NODES > 8)) {
    fprintf(stderr, "Invalid node number given.\n");
    exit(1);
  } else if ((start_port < 1024) || start_port >= (MAX_PORTNUM - TOTAL_NODES)) {
    fprintf(stderr, "Invalid starting port given.\n");
    exit(1);
  }

  NODES = calloc(TOTAL_NODES, sizeof(node_info_extended));
  parent_connfd = get_listenfd(&start_port);
  PARENT_PORT = start_port;

  for (int n = 0; n < TOTAL_NODES; n++) {
    start_port++; // start search at previously assigned port + 1
    n_connfd = get_listenfd(&start_port);
    if (n_connfd < 0) {
      fprintf(stderr, "get_listenfd error\n");
      exit(1);
    }
    NODES[n].base_info.listen_fd = n_connfd;
    NODES[n].base_info.node_id = n;
    NODES[n].base_info.port_number = start_port;
  }

  // Begin forking all child processes.
  for (int n = 0; n < TOTAL_NODES; n++) {
    if ((pid = Fork()) == 0) { // child process
      Close(parent_connfd);
      start_node(n);
      exit(1);
    } else {
      node_info_extended node = NODES[n];
      fprintf(stderr, "NODE %d [PID: %d] listening on port %d\n", n, pid, node.base_info.port_number);
    }
  }

  // Parent closes all fd's that belong to it's children
  for (int n = 0; n < TOTAL_NODES; n++)
    Close(NODES[n].base_info.listen_fd);

  // Parent can now begin waiting for children to send messages to contact.
  parent_serve((char *) argv[3], parent_connfd);
  Close(parent_connfd);

  parent_end();

  return 0;
}