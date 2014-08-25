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
#include <sys/select.h>

#include <termios.h>
#include <curses.h>

#define LEN_FIELD_SIZE 4
#define MAX_CLIENTS 32

char *process_message(int sock_fd);
void send_message(int sock_fd, const char *buf);

void start_server_loop(const char *port);
void *transmit_thread(void *unused);

void write_in_window(const char *message, ...);
void clear_window();

/* Current window line */
int current_line, window_height, window_width;

/* Inter-thread communication variables */
pthread_mutex_t draw_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t client_list_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t copy_buffer_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t copy_buffer_cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t transmitted_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t transmitted_cond = PTHREAD_COND_INITIALIZER;

int copy_buffer_flag = -1, transmitted_flag = -1;
char *copy_buffer;

typedef struct _client_data {
    int sock_fd;
    char *username;
} client_data_t;

client_data_t clients[MAX_CLIENTS];
int clients_counter = 0;

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

void start_server_loop(const char *port) {
    struct sockaddr_in local_address;
    
    /* Specify socket parameters */
    local_address.sin_family = AF_INET;
    local_address.sin_port = htons(atoi(port));
    local_address.sin_addr.s_addr = INADDR_ANY;
    
    /* Create a TCP socket */
    int listen_fd;
    if ((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        exit(-1);
    }
    
    /* Allow socket reusing */
    int value = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(int)) == -1) {
        perror("setsockopt");
        exit(-1);
    }
    
    /* Bind the socket */
    if (bind(listen_fd, (struct sockaddr *) &local_address, sizeof(local_address)) == -1) {
        perror("bind");
        exit(-1);
    }
    
    /* Start listening */
    if (listen(listen_fd, 2) == -1) {
        perror("listen");
        exit(-1);
    }
    
    write_in_window("[info] Started listening");
    
    pthread_t transmit_handle;
    pthread_create(&transmit_handle, NULL, transmit_thread, NULL);
    
    struct sockaddr_in remote_address;
    socklen_t remote_address_size = sizeof(remote_address);

    fd_set all_sockets, ready_sockets;
    int max_fd;

    FD_SET(listen_fd, &all_sockets);
    max_fd = listen_fd;
    
    while (1) {
        ready_sockets = all_sockets;
        if (select(max_fd + 1, &ready_sockets, NULL, NULL, NULL) == -1) {
            perror("select");
            exit(1);
        }
        
        int i;
        for (i = 0; i <= max_fd; i++) {
            if (FD_ISSET(i, &ready_sockets)) {
                if (i == listen_fd) {
                    /* New connection waiting */
                    int new_sock_fd = accept(listen_fd, (struct sockaddr *) &remote_address, &remote_address_size);
                    char *username = process_message(new_sock_fd);
                    
                    if ((clients_counter + 1) == MAX_CLIENTS) {
                        /* Max amount of clients reached */
                        send_message(new_sock_fd, "Too many clients!");
                        close(new_sock_fd);
                        free(username);
                        continue;
                    }
                    
                    /* Add to socket list */
                    FD_SET(new_sock_fd, &all_sockets);
                    
                    /* Keep track of maximum fd value */
                    if (new_sock_fd > max_fd)
                        max_fd = new_sock_fd;
                    
                    pthread_mutex_lock(&client_list_mutex);
                        clients[clients_counter].sock_fd = new_sock_fd;
                        clients[clients_counter].username = username;
                        clients_counter++;
                    pthread_mutex_unlock(&client_list_mutex);
                } else {
                    /* Client wants to send data */
                    copy_buffer = process_message(i);
                    
                    pthread_mutex_lock(&copy_buffer_mutex);
                        /* The transmit thread needs the origin of the message */
                        copy_buffer_flag = i;
                    
                        /* Signal the transmitter to start */
                        pthread_cond_signal(&copy_buffer_cond);
                    pthread_mutex_unlock(&copy_buffer_mutex);
                    
                    pthread_mutex_lock(&transmitted_mutex);
                        /* Wait for the transmitter to finish */
                        while (!transmitted_flag)
                            pthread_cond_wait(&transmitted_cond, &transmitted_mutex);
                    
                        /* Reset the flag */
                        transmitted_flag = FALSE;
                    pthread_mutex_unlock(&transmitted_mutex);
                    
                    /* Print the message on the server */
                    pthread_mutex_lock(&draw_mutex);
                        write_in_window(copy_buffer);
                    pthread_mutex_unlock(&draw_mutex);
                    
                    free(copy_buffer);
                }
            }
        }
    }
    
    
    /* Close listening socket */
    close(listen_fd);
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

void *transmit_thread(void *unused) {
    while (1) {
        pthread_mutex_lock(&copy_buffer_mutex);
            /* Wait for a buffer to become available */
            while (copy_buffer_flag == -1)
                pthread_cond_wait(&copy_buffer_cond, &copy_buffer_mutex);

            pthread_mutex_lock(&client_list_mutex);
                int i;
                for (i = 0; i < clients_counter; ++i)
                    /* copy_buffer_flag holds the originating socket; don't repeat the message there */
                    if (clients[i].sock_fd != copy_buffer_flag)
                        send_message(clients[i].sock_fd, copy_buffer);
            pthread_mutex_unlock(&client_list_mutex);
        
            /* Clear the flag */
            copy_buffer_flag = -1;
        pthread_mutex_unlock(&copy_buffer_mutex);
        
        pthread_mutex_lock(&transmitted_mutex);
            transmitted_flag = TRUE;
            /* Signal the main thread that we have finished transmitting */
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
    start_server_loop(argv[1]);
    
    endwin();
    return 0;
}