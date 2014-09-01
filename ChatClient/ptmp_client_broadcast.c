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

char *username;

#define write_in_chat_window(m, ...) write_in_window(chat_window, &current_chat_line, chat_height, m, ##__VA_ARGS__)
#define write_in_input_window(m, ...) write_in_window(input_window, &current_input_line, input_height, m, ##__VA_ARGS__)

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

int connect_client(const char *host, const char *port) {
    int sock_fd;
    struct addrinfo hints, *result;
    
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    
    getaddrinfo(host, port, &hints, &result);
    
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
    wmove(win, *current_line, 1);
    vwprintw(win, message, args);
    va_end(args);
    
    if (*current_line != window_height)
        (*current_line)++;
    else
        scroll(win);
    
    wrefresh(win);
}

void clear_window(WINDOW *win) {
    werase(win);
    box(win, '|', '=');
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
    char *formatted_data;
    int sock_fd = *((int *) sock_fd_ptr);
    
    while (1) {
        memset(input_buffer, 0, 1024);
        mvwgetstr(input_window, current_input_line, 1, input_buffer);
        
        asprintf(&formatted_data, "[%s] %s", username, input_buffer);
        
        send_message(sock_fd, formatted_data);
        
        pthread_mutex_lock(&draw_mutex);
        write_in_chat_window(formatted_data);
        clear_window(input_window);
        pthread_mutex_unlock(&draw_mutex);
    }
    
    free(input_buffer);
    free(formatted_data);
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

void search_servers(const char *network, const char *port) {
    int sockfd, broadcast = 1;
    struct addrinfo hints, *result;
    
    /* Specify required socket */
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;
    
    /* Get an addrinfo struct for the broadcast address */
    getaddrinfo(network, port, &hints, &result);
    
    /* Init socket */
    sockfd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    
    /* Allow broadcasting */
    setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    
    /* Broadcast 0x7F 0x7F */
    sendto(sockfd, "\x7f\x7f", 2, 0, result->ai_addr, result->ai_addrlen);
    
    /* Wait for replies */
    struct sockaddr_in sender_address;
    char address[INET_ADDRSTRLEN];
    socklen_t addr_len = sizeof(sender_address);
    char *buf = malloc(32);
    int bytes_received;
    
    /* Set a 5 second receive timeout on the socket */
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    /* Loop until recvfrom returns an error */
    while ((bytes_received = recvfrom(sockfd, buf, 32, 0, &sender_address, &addr_len)) > 0)
        write_in_chat_window("Received reply from %s: %s\n", inet_ntop(sender_address.sin_family, &sender_address.sin_addr, address, sizeof(address)), buf);
    
    write_in_chat_window("Timed out - press any key\n");
    wgetch(input_window);
    
    close(sockfd);
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
    
    if (argc >= 3) {
        if (strcmp(argv[1], "-b") == 0) {
            /* Broadcast server lookup message */
            search_servers(argv[2], argv[3]);
        } else {
            /* Init network connection */
            int sock_fd;
            sock_fd = connect_client(argv[1], argv[2]);
            
            /* Send username to server */
            write_in_input_window("Enter username: ");
            username = (char *) malloc(32);
            wgetnstr(input_window, username, 32);
            send_message(sock_fd, username);
            
            /* Clear input window */
            clear_window(input_window);
            
            /* Create I/O threads */
            pthread_t send_thread, receive_thread;
            pthread_attr_t joinable_attr;
            pthread_attr_init(&joinable_attr);
            pthread_attr_setdetachstate(&joinable_attr, PTHREAD_CREATE_JOINABLE);
            
            int *sock_fd_ptr = &sock_fd;
            pthread_create(&send_thread, &joinable_attr, send_thread_loop, (void *) sock_fd_ptr);
            pthread_create(&receive_thread, &joinable_attr, receive_thread_loop, (void *) sock_fd_ptr);
            
            pthread_join(send_thread, NULL);
            pthread_join(receive_thread, NULL);
            
            free(username);
        }
    } else {
        fprintf(stderr, "Incorrect number of arguments\n");
        return 1;
    }
    
    endwin();

    return 0;
}

