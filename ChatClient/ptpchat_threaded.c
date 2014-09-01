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

char *process_message(int sock_fd);
void send_message(int sock_fd, const char *buf);
void write_in_window(WINDOW *win, int *current_line, int window_height, const char *message, ...);

/* UI stuff */
WINDOW *chat_window;
WINDOW *input_window;

int chat_height, input_height;
int current_chat_line, current_input_line;

/* Inter-thread communication variables */
pthread_mutex_t draw_mutex = PTHREAD_MUTEX_INITIALIZER;

#define write_in_chat_window(m, ...) write_in_window(chat_window, &current_chat_line, chat_height, m, ##__VA_ARGS__)
#define write_in_input_window(m, ...) write_in_window(input_window, &current_window_line, window_height, m, ##__VA_ARGS__)

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
        write_in_chat_window("[info] Connection closed\n");
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

int start_server(const char *port) {
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
    
    write_in_chat_window("[info] Listening on 0.0.0.0:%s\n", port);
    
    /* Wait for a connection */
    remote_address_size = sizeof(remote_address);
    new_sock_fd = accept(sock_fd, (struct sockaddr *) &remote_address, &remote_address_size);
    
    write_in_chat_window("[info] Received connection");
    
    /* Close listening socket */
    close(sock_fd);
    
    return new_sock_fd;
}

int connect_client(const char *hostname, const char *port) {
    int sock_fd;
    struct addrinfo hints, *result;
    
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    
    getaddrinfo(hostname, port, &hints, &result);
    
    sock_fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (connect(sock_fd, result->ai_addr, result->ai_addrlen) == -1) {
        perror("connect");
        exit(-1);
    }
    
    write_in_chat_window("[info] Connected\n");
    
    freeaddrinfo(result);
    
    return sock_fd;
}

void write_in_window(WINDOW *win, int *current_line, int window_height, const char *message, ...) {
    va_list args;
    va_start(args, message);
    move(*current_line, 1);
    vwprintw(win, message, args);
    va_end(args);
    
    if (*current_line != window_height)
        (*current_line)++;
    else
        scroll(win);
    
    wrefresh(win);
}

void *send_thread_loop(void *sock_fd_ptr) {
    /* Wait for user input;
     * Assemble message;
     * Send;
     * Acquire draw mutex, draw, release
     * Repeat
     */
    char *input_buffer = (char *) malloc(1024);
    int sock_fd = *((int *) sock_fd_ptr);
    
    while (1) {
        memset(input_buffer, 0, 1024);
        mvwgetstr(input_window, current_input_line, 2, input_buffer);
        
        send_message(sock_fd, input_buffer);
        
        pthread_mutex_lock(&draw_mutex);
            write_in_chat_window(input_buffer);
            werase(input_window);
            box(input_window, '|', '=');
            wrefresh(input_window);
        pthread_mutex_unlock(&draw_mutex);
    }
    
    free(input_buffer);
}

void *receive_thread_loop(void *sock_fd_ptr) {
    /* process_message
     * Acquire draw mutex, draw, release
     * Repeat
     */
    int sock_fd = *((int *) sock_fd_ptr);
    
    while (1) {
        char *rcvd_msg = process_message(sock_fd);
        
        pthread_mutex_lock(&draw_mutex);
            write_in_chat_window(rcvd_msg);
        pthread_mutex_unlock(&draw_mutex);
        
        free(rcvd_msg);
    }
}

int main(int argc, const char * argv[]) {
    /* Init ncurses */
    initscr();
    
    /* Retrieve window dimensions */
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    
    /* Set top and bottom window sizes */
    chat_height = max_y * (8.0f/10.0f);
    input_height = max_y * (2.0f/10.0f);
    
    chat_window = newwin(chat_height, max_x, 0, 0);
    input_window = newwin(input_height, max_x, chat_height, 0);
    
    /* Current lines on both windows is 1 */
    current_chat_line = current_input_line = 1;
    
    /* Enable scrolling on the windows */
    scrollok(chat_window, TRUE);
    scrollok(input_window, TRUE);
    
    /* Draw the borders */
    box(chat_window, '|', '=');
    box(input_window, '|', '=');
    
    /* Specify the scrolling region in the windows, taking borders into account */
    wsetscrreg(chat_window, 1, chat_height - 2);
    wsetscrreg(input_window, 1, input_height - 2);
    
    /* Draw the windows */
    wrefresh(chat_window);
    wrefresh(input_window);
    
    /* Init network connection */
    int sock_fd;
    if (argc <= 2)
        sock_fd = start_server(argv[1]);
    else
        sock_fd = connect_client(argv[1], argv[2]);
    
    pthread_t send_thread, receive_thread;
    pthread_attr_t joinable_attr;
    pthread_attr_init(&joinable_attr);
    pthread_attr_setdetachstate(&joinable_attr, PTHREAD_CREATE_JOINABLE);
    
    int *sock_fd_ptr = &sock_fd;
    pthread_create(&send_thread, &joinable_attr, send_thread_loop, (void *) sock_fd_ptr);
    pthread_create(&receive_thread, &joinable_attr, receive_thread_loop, (void *) sock_fd_ptr);
    
    pthread_join(send_thread, NULL);
    pthread_join(receive_thread, NULL);
    
    endwin();
    return 0;
}

