//
//  main.c
//  ChatClient
//
//  Created by Itamar Ravid on 22/8/14.
//  Copyright (c) 2014 Itamar Ravid. All rights reserved.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <pthread.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

#include <termios.h>
#include <curses.h>

#define LEN_FIELD_SIZE 4
#define MAX_CLIENTS 32

typedef struct _thread_data_t {
    int client_id;
    int sock_fd;
    char *username;
    char *transmit_buffer;
} thread_data_t;

typedef struct _broadcast_data_t {
    char *port;
    char *room_name;
} broadcast_data_t;

char *process_message(int sock_fd);
void send_message(int sock_fd, const char *buf);

void start_server_loop(const char *port, const char *room_name);
pthread_t spawn_client_thread(thread_data_t *thread_data);
void *client_thread_loop(void *sock_fd_ptr);
void *transmit_thread(void *unused);

void write_in_window(const char *message, ...);
void clear_window();

/* Client threads */
pthread_t client_threads[MAX_CLIENTS];
thread_data_t client_data[MAX_CLIENTS];
int client_counter;

/* Current window line */
int current_line, window_height, window_width;

/* Inter-thread communication variables */
pthread_mutex_t draw_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t client_list_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t copy_buffer_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t copy_buffer_cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t transmitted_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t transmitted_cond = PTHREAD_COND_INITIALIZER;

int copy_from = -1, transmitted_from = -1;

void pack_32i(uint32_t value, char *buffer) {
    *buffer = value >> 24;
    *(buffer + 1) = value >> 16;
    *(buffer + 2) = value >> 8;
    *(buffer + 3) = value;
}

uint32_t unpack_32i(char *buffer) {
    return (*(buffer + 3)) | (*(buffer + 2) << 8) | (*(buffer + 1) << 16) | (*buffer << 24);
}

void send_message(int sock_fd, const char *data) {
    char *msg = NULL;
    
    /* Compute lengths, account for NUL in data_len */
    uint32_t data_len = strlen(data) + 1;
    uint32_t msg_len = data_len + LEN_FIELD_SIZE;
    
    /* Allocate msg buffer and set the length */
    msg = (char *) malloc(msg_len);
    pack_32i(msg_len, msg);
    
    /* Copy data to msg buffer */
    memcpy(msg + LEN_FIELD_SIZE, data, data_len);
    
    /* Write data to wire */
    int bytes_written = 0, bytes_left = msg_len - bytes_written, total = 0;
    while (bytes_left > 0) {
        bytes_written = send(sock_fd, msg + total, bytes_left, 0);
        bytes_left -= bytes_written;
        total += bytes_left;
    }
}

char *process_message(int sock_fd) {
    /* Message structure:
     * <length> <data>
     * Length includes all of the other fields and itself. It is a 32-bit integer.
     * Data is the data. Ignored if type == 1.
     */
    
    /* Working buffer */
    char *sock_buf = (char *) malloc(1024);
    
    /* Another buffer we'll use later for the data field */
    char *data_buf;
    
    /* Start by receiving LEN_FIELD_SIZE bytes to get the next message length */
    memset(sock_buf, 0, 1024);
    if (recv(sock_fd, sock_buf, LEN_FIELD_SIZE, 0) == 0) {
        write_in_window("[info] Connection closed\n");
        exit(0);
    }
    
    /* Unpack length */
    uint32_t msg_len = unpack_32i(sock_buf);
    uint32_t data_len = msg_len - LEN_FIELD_SIZE; /* Substract 4 bytes to get the data length */
    
    /* Allocate another buffer for the message, take NUL into account */
    data_buf = (char *) malloc(data_len + 1);
    /* Various counters */
    int bytes_read = 0, bytes_left = data_len - bytes_read, total = 0;
    
    while (bytes_left > 0) {
        bytes_read = recv(sock_fd, sock_buf, 1024, 0);
        
        memcpy(data_buf + total, sock_buf, bytes_read);
        
        bytes_left -= bytes_read;
        total += bytes_read;
    }
    
    /* Add NUL-terminator */
    data_buf[total] = '\0';
    
    /* Free socket buffer */
    free(sock_buf);
    
    return data_buf;
}

void *broadcast_listener(void *arg) {
    broadcast_data_t *broadcast_data = (broadcast_data_t *) arg;
    
    /* Pad the room name to 32 bytes */
    char *padded_room_name = malloc(32);
    strcpy(padded_room_name, broadcast_data->room_name);
    memset((padded_room_name + strlen(padded_room_name)), '\0', 32 - strlen(padded_room_name));
    
    /* Open the listener socket */
    int sockfd;
    struct addrinfo hints, *result;
    
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;
    
    getaddrinfo(NULL, broadcast_data->port, &hints, &result);
    
    sockfd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    
    bind(sockfd, result->ai_addr, result->ai_addrlen);
    
    struct sockaddr_in sender_address;
    socklen_t addr_len = sizeof(sender_address);
    
    while (1) {
        /* Allocate two bytes for 0x7F 0x7F */
        char *buf = malloc(2);
        
        /* Read two bytes from socket */
        int bytes_received = recvfrom(sockfd, buf, 2, 0, &sender_address, &addr_len);
        
        /* Check that the two bytes are indeed 0x7F 0x7F */
        if (buf[0] == '\x7f' && buf[1] == '\x7f')
            sendto(sockfd, padded_room_name, 32, 0, (struct sockaddr *) &sender_address, addr_len);
        
        free(buf);
    }
}

void start_server_loop(const char *port, const char *room_name) {
    struct sockaddr_in local_address, remote_address;
    socklen_t remote_address_size;
    
    int sock_fd, new_sock_fd;
    
    /* Specify socket parameters */
    local_address.sin_family = AF_INET;
    local_address.sin_port = htons(atoi(port));
    local_address.sin_addr.s_addr = INADDR_ANY;
    
    /* Create a TCP socket */
    if ((sock_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        exit(-1);
    }
    
    /* Allow socket reusing */
    int value = 1;
    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(int)) == -1) {
        perror("setsockopt");
        exit(-1);
    }
    
    /* Bind the socket */
    if (bind(sock_fd, (struct sockaddr *) &local_address, sizeof(local_address)) == -1) {
        perror("bind");
        exit(-1);
    }
    
    /* Start listening */
    if (listen(sock_fd, 2) == -1) {
        perror("listen");
        exit(-1);
    }
    
    write_in_window("[info] Started listening");
    
    pthread_t transmit_handle;
    pthread_create(&transmit_handle, NULL, transmit_thread, NULL);
    
    broadcast_data_t *broadcast_data = malloc(sizeof(broadcast_data));
    broadcast_data->port = port;
    broadcast_data->room_name = room_name;
    
    pthread_t broadcast_handle;
    pthread_create(&broadcast_handle, NULL, broadcast_listener, (void *) broadcast_data);
    
    client_counter = 0;
    
    /* Connection handling loop */
    while (client_counter < MAX_CLIENTS) {
        remote_address_size = sizeof(remote_address);
        client_data[client_counter].sock_fd = accept(sock_fd, (struct sockaddr *) &remote_address, &remote_address_size);
        
        /* Accept the username message */
        client_data[client_counter].username = process_message(client_data[client_counter].sock_fd);
        client_data[client_counter].client_id = client_counter;
        
        write_in_window("[info] Received connection");
        
        /* Lock the list and counter so the transmit thread does not use them concurrently */
        pthread_mutex_lock(&client_list_mutex);
        client_threads[client_counter] = spawn_client_thread(&client_data[client_counter]);
        client_counter++;
        pthread_mutex_unlock(&client_list_mutex);
    }
    
    /* Wait for threads to finish */
    int i;
    for (int i = 0; i < MAX_CLIENTS; i++)
        pthread_join(client_threads[i], NULL);
    
    /* Close listening socket */
    close(sock_fd);
}

pthread_t spawn_client_thread(thread_data_t *thread_data) {
    pthread_attr_t joinable_attr;
    pthread_attr_init(&joinable_attr);
    pthread_attr_setdetachstate(&joinable_attr, TRUE);
    
    pthread_t thread_handle;
    pthread_create(&thread_handle, &joinable_attr, client_thread_loop, (void *) thread_data);
    
    return thread_handle;
}

void write_in_window(const char *message, ...) {
    va_list args;
    va_start(args, message);
    move(current_line, 1);
    vwprintw(stdscr, message, args);
    va_end(args);
    
    if (current_line != window_height)
        current_line++;
    else
        scroll(stdscr);
    
    wrefresh(stdscr);
}

void *client_thread_loop(void *thread_data) {
    /* process_message
     * Acquire draw mutex, draw, release
     * Repeat
     */
    thread_data_t *data = (thread_data_t *) thread_data;
    
    while (1) {
        data->transmit_buffer = process_message(data->sock_fd);
        
        pthread_mutex_lock(&copy_buffer_mutex);
        copy_from = data->client_id;
        pthread_cond_signal(&copy_buffer_cond);
        pthread_mutex_unlock(&copy_buffer_mutex);
        
        pthread_mutex_lock(&transmitted_mutex);
        while (transmitted_from != data->client_id)
            pthread_cond_wait(&transmitted_cond, &transmitted_mutex);
        
        transmitted_from = -1;
        pthread_mutex_unlock(&transmitted_mutex);
        
        pthread_mutex_lock(&draw_mutex);
        write_in_window(data->transmit_buffer);
        pthread_mutex_unlock(&draw_mutex);
        
        free(data->transmit_buffer);
    }
}

void *transmit_thread(void *unused) {
    int saved_copy_from;
    
    while (1) {
        pthread_mutex_lock(&copy_buffer_mutex);
        while (copy_from == -1)
            pthread_cond_wait(&copy_buffer_cond, &copy_buffer_mutex);
        
        pthread_mutex_lock(&client_list_mutex);
        int i;
        for (i = 0; i < client_counter; i++)
            if (i != copy_from)
                send_message(client_data[i].sock_fd, client_data[copy_from].transmit_buffer);
        pthread_mutex_unlock(&client_list_mutex);
        
        saved_copy_from = copy_from;
        copy_from = -1;
        pthread_mutex_unlock(&copy_buffer_mutex);
        
        pthread_mutex_lock(&transmitted_mutex);
        transmitted_from = saved_copy_from;
        pthread_cond_signal(&transmitted_cond);
        pthread_mutex_unlock(&transmitted_mutex);
    }
}

void clear_window(WINDOW *win) {
    werase(win);
    box(win, '|', '=');
    wrefresh(win);
}

int main(int argc, const char * argv[]) {
    /* Init ncurses */
    initscr();
    
    /* Retrieve dimensions */
    getmaxyx(stdscr, window_height, window_width);
    
    /* Current line is 1 */
    current_line = 1;
    
    /* Enable scrolling on the window */
    scrollok(stdscr, TRUE);
    
    /* Specify the scrolling region in the window, taking borders into account */
    wsetscrreg(stdscr, 1, window_height - 2);
    
    /* Erase window and draw the borders */
    clear_window(stdscr);
    
    /* Draw the windows */
    wrefresh(stdscr);
    
    /* Start listen loop */
    if (argc == 3)
        start_server_loop(argv[1], argv[2]);
    else {
        write_in_window("Two arguments needed - press any key to end\n");
        wgetch(stdscr);
    }
    
    endwin();
    return 0;
}

