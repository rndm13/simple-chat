#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEFAULT_PORT 8080
#define MAX_HOSTNAME_SIZE 128
#define DEFAULT_HOST "127.0.0.1"
#define MAX_CONNECTION_COUNT 10
#define MAX_MESSAGE_SIZE 256
#define MAX_USER_NAME_SIZE 64
#define MAX_PACKET_SIZE 1400

#define ARRAY_SIZE(X) sizeof(X) / sizeof(*X)

int i32min(int a, int b) {
    return a + (b - a) * (b < a);
}

int i32max(int a, int b) {
    return a + (b - a) * (b > a);
}

void str_right_trim(char *str, int str_size) {
    for (int i = str_size - 1; i >= 0; i--) {
        if (str[i] == '\0') {
            continue;
        }

        if (str[i] == '\n' || str[i] == ' ' || str[i] == '\t') {
            str[i] = '\0';
        } else {
            break;
        }
    }
}

typedef enum ERRNO {
    ERRNO_ADDRESS_ALREADY_IN_USE = 98,
    ERRNO_TRANSPORT_ENDPOINT_IS_NOT_CONNECTED = 107,
} ERRNO;

typedef enum RequestType : uint8_t {
    REQUEST_TYPE_LOGIN = 0,
    REQUEST_TYPE_MESSAGE = 1,
} RequestType;

typedef struct RequestHeader {
    int size;
    RequestType type;
} RequestHeader;

typedef union RequestData {
    struct {
        char user_name[MAX_USER_NAME_SIZE];
    } login;
    struct {
        char message[MAX_MESSAGE_SIZE];
    } message;
} RequestData;

typedef struct Request {
    RequestHeader header;
    RequestData data;
} Request;

typedef enum ResponseType : uint8_t {
    RESPONSE_TYPE_ERROR = 0,
    RESPONSE_TYPE_USER_JOIN = 1,
    RESPONSE_TYPE_USER_DISCONNECT = 2,
    RESPONSE_TYPE_USER_MESSAGE = 3,
} ResponseType;

Request init_rq_login(char *name) {
    int size = strnlen(name, MAX_USER_NAME_SIZE);

    Request rq;
    memset(&rq, 0, sizeof(rq));

    rq.header.size = size;
    rq.header.type = REQUEST_TYPE_LOGIN;

    memcpy(rq.data.login.user_name, name, size);

    return rq;
}

Request init_rq_message(char *message) {
    int size = strnlen(message, MAX_MESSAGE_SIZE);

    Request rq;
    memset(&rq, 0, sizeof(rq));

    rq.header.size = size;
    rq.header.type = REQUEST_TYPE_MESSAGE;

    memcpy(rq.data.message.message, message, size);

    return rq;
}

typedef struct ResponseHeader {
    int size;
    ResponseType type;
} ResponseHeader;

#define MAX_ERROR_INFO_SIZE 128

typedef union ResponseData {
    struct {
        char info[MAX_ERROR_INFO_SIZE];
    } error;
    struct {
        char user_name[MAX_USER_NAME_SIZE];
    } user_join_disconnect;
    struct {
        char user_name[MAX_USER_NAME_SIZE];
        char message[MAX_MESSAGE_SIZE];
    } user_message;
} ResponseData;

typedef struct Response {
    ResponseHeader header;
    ResponseData data;
} Response;

Request init_rs_login(char *name) {
    int size = strnlen(name, MAX_USER_NAME_SIZE);

    Request rq;
    memset(&rq, 0, sizeof(rq));

    rq.header.size = size;
    rq.header.type = REQUEST_TYPE_LOGIN;

    memcpy(rq.data.login.user_name, name, size);

    return rq;
}

Response init_rs_error(char *info, ...) {
    va_list val;
    va_start(val, info);

    Response rs;
    memset(&rs, 0, sizeof(rs));

    int info_size = vsnprintf(rs.data.error.info, MAX_ERROR_INFO_SIZE, info, val);

    rs.header.size = info_size;
    rs.header.type = RESPONSE_TYPE_ERROR;

    return rs;
}

Response init_rs_join_disconnect(char *user_name, bool join) {
    int user_name_size = strnlen(user_name, MAX_USER_NAME_SIZE);

    Response rs;
    memset(&rs, 0, sizeof(rs));

    rs.header.size = user_name_size;
    rs.header.type = join ? RESPONSE_TYPE_USER_JOIN : RESPONSE_TYPE_USER_DISCONNECT;

    memcpy(rs.data.user_join_disconnect.user_name, user_name, user_name_size);

    return rs;
}

Response init_rs_message(char *user_name, char *message) {
    int message_size = strnlen(message, MAX_MESSAGE_SIZE);
    int user_name_size = strnlen(user_name, MAX_USER_NAME_SIZE);

    Response rs;
    memset(&rs, 0, sizeof(rs));

    rs.header.size = message_size + user_name_size;
    rs.header.type = RESPONSE_TYPE_USER_MESSAGE;

    memcpy(rs.data.user_message.message, message, message_size);
    memcpy(rs.data.user_message.user_name, user_name, user_name_size);

    return rs;
}

int read_completely(int fd, void *bytes, int size) {
    int read_bytes = 0;

    while (read_bytes < size) {
        int just_read = read(fd, bytes + read_bytes, i32min(size - read_bytes, MAX_PACKET_SIZE));

        if (just_read == 0) {
            return -1;
        }
        if (just_read < 0) {
            return -2;
        }

        read_bytes += just_read;
    }

    return read_bytes;
}

int write_completely(int fd, void *bytes, int size) {
    int wrote_bytes = 0;
    while (wrote_bytes < size) {
        int just_wrote = write(fd, bytes + wrote_bytes, i32min(size - wrote_bytes, MAX_PACKET_SIZE));

        if (just_wrote < 0) {
            return -1;
        }

        wrote_bytes += just_wrote;
    }

    return wrote_bytes;
}

typedef struct Connection {
    int sock;
    struct sockaddr_in addr;

    char user_name[MAX_USER_NAME_SIZE];
} Connection;

void reset_connection(Connection *conn) {
    conn->sock = -1;
    memset(&conn->addr, 0, sizeof(conn->addr));
    memset(&conn->user_name, 0, sizeof(conn->user_name));
}

typedef struct ServerState {
    int sock_server;
    struct sockaddr_in addr_server;

    Connection connections[MAX_CONNECTION_COUNT];
    int connection_count;
} ServerState;

int server_init(ServerState *server, const char *host, int port) {
    signal(SIGPIPE, SIG_IGN);

    server->sock_server = socket(AF_INET, SOCK_STREAM, 0);
    if (server->sock_server < 0) {
        printf("ERROR: Socket %d\n", errno);
        return -1;
    }

    memset(&server->addr_server, 0, sizeof(server->addr_server));

    server->addr_server.sin_family = AF_INET;
    server->addr_server.sin_addr.s_addr = inet_addr(DEFAULT_HOST);

    int actual_port = port;
    int err;
    do {
        if (actual_port > port) {
            printf("WARN: Binding to port %d failed, trying %d\n", actual_port - 1, actual_port);
        }
        server->addr_server.sin_port = htons(actual_port);

        int err = bind(server->sock_server, (struct sockaddr *)&server->addr_server, sizeof(server->addr_server));

        actual_port++;
    } while (err < 0 && errno == ERRNO_ADDRESS_ALREADY_IN_USE);
    actual_port--;

    if (err < 0) {
        printf("ERROR: Bind %d\n", errno);
        return -2;
    }

    printf("INFO: Bound server to port %s:%d\n", host, actual_port);

    err = listen(server->sock_server, MAX_CONNECTION_COUNT);
    if (err < 0) {
        printf("ERROR: Listen %d\n", errno);
        return -3;
    }

    for (int i = 0; i < ARRAY_SIZE(server->connections); i++) {
        reset_connection(&server->connections[i]);
    }
    server->connection_count = 0;

    return 0;
}

int server_send_everyone(ServerState *server, Response rs, int except) {
    int count = 0;

    for (int i = 0; i < MAX_CONNECTION_COUNT; i++) {
        if (i == except) {
            continue;
        }

        write_completely(server->connections[i].sock, &rs, sizeof(rs));

        count++;
    }

    return count;
}

int server_start(ServerState *server) {
    printf("INFO: Started server\n");

    while (true) {
        struct pollfd polls[MAX_CONNECTION_COUNT + 1];
        memset(polls, 0, sizeof(polls));

        polls[0].fd = server->sock_server;
        polls[0].events = POLLIN;

        // clean up disconnected clients
        for (int i = 0; i < server->connection_count; i++) {
            if (server->connections[i].sock < 0) {
                printf("DEBUG: Cleaned up connection %d\n", i);
                int last = server->connection_count - 1;

                if (last != i) {
                    memcpy(&server->connections[i], &server->connections[last], sizeof(Connection));
                }

                reset_connection(&server->connections[last]);

                server->connection_count = i32max(server->connection_count - 1, 0);
            }
        }

        for (int i = 0; i < server->connection_count; i++) {
            if (server->connections[i].sock >= 0) {
                polls[i + 1].fd = server->connections[i].sock;
                polls[i + 1].events = POLLIN;
            }
        }

        printf("INFO: Polling events for %d file descriptors\n", server->connection_count + 1);
        int err = poll(polls, server->connection_count + 1, -1);
        if (err < 0) {
            printf("ERROR: Failed to poll new events %d, connection_count: %d\n", errno, server->connection_count);
        }
        printf("INFO: Processing new events\n");

        // New connections
        while (poll(&polls[0], 1, 0) > 0) {
            if (!(polls[0].revents & POLLIN)) {
                printf("ERROR: Unexpected accept revents: %d %d %d\n",
                       (bool)(polls[0].revents & POLLERR),
                       (bool)(polls[0].revents & POLLHUP),
                       (bool)(polls[0].revents & POLLNVAL));

                break;
            }

            printf("INFO: Accepting new connection\n");
            int new_conn_idx = server->connection_count;

            if (new_conn_idx >= ARRAY_SIZE(server->connections)) {
                // Send an error response
                struct sockaddr_in addr_conn;
                socklen_t addr_conn_len = sizeof(addr_conn);
                int sock_conn = accept(server->sock_server, (struct sockaddr *)&addr_conn, &addr_conn_len);

                if (sock_conn < 0) {
                    printf("ERROR: Failed to accept connection (send a connection limit message)\n");

                    continue;
                }

                Response rs = init_rs_error("Connection limit, please wait until someone disconnects");

                write_completely(sock_conn, &rs, sizeof(rs));

                close(sock_conn);

                continue;
            }

            socklen_t addr_connection_size = sizeof(server->connections[new_conn_idx].addr);
            server->connections[new_conn_idx].sock = accept(server->sock_server,
                                                            (struct sockaddr *)&server->connections[new_conn_idx].addr,
                                                            &addr_connection_size);

            if (server->connections[new_conn_idx].sock < 0) {
                printf("ERROR: Failed to accept connection %d\n", errno);

                continue;
            }

            polls[server->connection_count + 1].fd = server->connections[new_conn_idx].sock;
            polls[server->connection_count + 1].events = POLLIN;

            server->connection_count++;
            printf("INFO: Accepted new connection %d\n", server->connection_count);
        }

        // New reads from sockets
        for (int i = 0; i < server->connection_count; i++) {
            if (poll(polls + i + 1, 1, 0) > 0) {
                Request rq;
                memset(&rq, 0, sizeof(rq));

                int err = read_completely(polls[i + 1].fd, &rq, sizeof(rq));

                if (err <= 0) {
                    char user_name[MAX_USER_NAME_SIZE];
                    // Disconnect
                    for (int j = 0; i < ARRAY_SIZE(server->connections); j++) {
                        if (server->connections[j].sock == polls[i + 1].fd) {
                            memcpy(user_name, server->connections[j].user_name, MAX_USER_NAME_SIZE);
                            close(server->connections[j].sock);

                            reset_connection(&server->connections[j]);
                            break;
                        }
                    }

                    if (strnlen(user_name, MAX_USER_NAME_SIZE) > 0) {
                        Response rs = init_rs_join_disconnect(user_name, false);

                        server_send_everyone(server, rs, -1);
                    }

                    printf("ERROR: Connection %d failed to read: %d. Disconnected\n", i, errno);
                    continue;
                }
                switch (rq.header.type) {
                case REQUEST_TYPE_LOGIN: {
                    printf("INFO: User %s has connected via client %d\n", rq.data.login.user_name, i);

                    Response rs = init_rs_join_disconnect(rq.data.login.user_name, true);
                    memcpy(server->connections[i].user_name, rq.data.login.user_name, MAX_USER_NAME_SIZE);

                    server_send_everyone(server, rs, -1);
                } break;
                case REQUEST_TYPE_MESSAGE: {
                    printf("INFO: User %s wrote '%s'\n", server->connections[i].user_name, rq.data.message.message);

                    Response rs = init_rs_message(server->connections[i].user_name, rq.data.message.message);

                    server_send_everyone(server, rs, -1);
                } break;
                default: {
                    printf("ERROR: Client %d sent an unknown type of request %d\n", i, rq.header.type);

                    Response rs = init_rs_error("Unknown type of request %d", rq.header.type);

                    int err = write_completely(polls[i + 1].fd, &rs, sizeof(rs));
                    if (err < 0) {
                        printf("ERROR: Failed to send error message to client %d", i);
                    }
                } break;
                }
            }
        }
    }

    return 0;
}

int server_deinit(ServerState *server) {
    printf("INFO: Closing server\n");
    for (int i = 0; i < MAX_CONNECTION_COUNT; i++) {
        if (server->connections[i].sock >= 0) {
            close(server->connections[i].sock);
        }
    }
    close(server->sock_server);

    return 0;
}

typedef struct ClientState {
    int sock_client;
    struct sockaddr_in addr_client;
} ClientState;

int client_init(ClientState *client, const char *hostname, int port) {
    client->sock_client = socket(AF_INET, SOCK_STREAM, 0);

    if (client->sock_client < 0) {
        printf("ERROR: socket %d\n", errno);
        return -1;
    }

    memset(&client->addr_client, 0, sizeof(client->addr_client));

    client->addr_client.sin_family = AF_INET;
    client->addr_client.sin_addr.s_addr = inet_addr(DEFAULT_HOST);
    client->addr_client.sin_port = htons(DEFAULT_PORT);

    int err = connect(client->sock_client, (struct sockaddr *)&client->addr_client, sizeof(client->addr_client));
    if (err < 0) {
        printf("ERROR: connect %d\n", errno);
        return -2;
    }

    printf("INFO: Connected to server\n");

    return 0;
}

int client_login(ClientState *client) {
    printf("Write your name:\n");
    char name[MAX_USER_NAME_SIZE + 1];
    memset(name, 0, MAX_USER_NAME_SIZE + 1);

    int read_bytes = read(STDIN_FILENO, name, MAX_USER_NAME_SIZE);
    if (read_bytes < 0) {
        printf("ERROR: Failed to read from stdin %d\n", errno);
    }
    str_right_trim(name, MAX_USER_NAME_SIZE);

    Request rq = init_rq_login(name);

    int err = write_completely(client->sock_client, &rq, sizeof(rq));
    if (err < 0) {
        printf("ERROR: Failed to write to server %d\n", errno);
        return -1;
    }

    return 0;
}

int client_start(ClientState *client) {
    int err = client_login(client);
    if (err < 0) {
        printf("ERROR: Failed to login\n");
        return err;
    }

    printf("You can now write messages to chat:\n");

    while (true) {
        const int POLL_COUNT = 2;
        struct pollfd polls[POLL_COUNT];

        // stdin
        polls[0].fd = STDIN_FILENO;
        polls[0].events = POLLIN;

        polls[1].fd = client->sock_client;
        polls[1].events = POLLIN;

        // Everything
        int err = poll(polls, POLL_COUNT, -1);
        if (err < 0) {
            printf("ERROR: Failed to poll new events %d\n", errno);
        }

        // stdin
        if (poll(&polls[0], 1, 0) > 0) {
            char message[MAX_MESSAGE_SIZE + 1];
            memset(message, 0, MAX_MESSAGE_SIZE + 1);

            int read_bytes = read(STDIN_FILENO, message, MAX_MESSAGE_SIZE);
            if (read_bytes < 0) {
                printf("ERROR: Failed to read from stdin %d\n", errno);
            }

            str_right_trim(message, MAX_MESSAGE_SIZE);
            Request rq = init_rq_message(message);

            int err = write_completely(client->sock_client, &rq, sizeof(rq));
            if (err < 0) {
                printf("ERROR: Failed to send a message\n");

                return -2;
            }
            printf("INFO: Successfully sent a message\n");
        }

        // socket
        if (poll(&polls[1], 1, 0) > 0) {
            Response rs;
            memset(&rs, 0, sizeof(rs));

            int err = read_completely(client->sock_client, &rs, sizeof(rs));
            if (err < 0) {
                printf("ERROR: Failed to read server response\n");

                return -3;
            }

            switch (rs.header.type) {
            case RESPONSE_TYPE_ERROR:
                printf("SERVER ERROR: %s\n", rs.data.error.info);
                break;
            case RESPONSE_TYPE_USER_JOIN:
                printf("SERVER INFO: user '%s' has joined the chat room\n", rs.data.user_join_disconnect.user_name);
                break;
            case RESPONSE_TYPE_USER_DISCONNECT:
                printf("SERVER INFO: user '%s' has disconnected from the chat room\n", rs.data.user_join_disconnect.user_name);
                break;
            case RESPONSE_TYPE_USER_MESSAGE:
                printf("%s: %s\n", rs.data.user_message.user_name, rs.data.user_message.message);
                break;
            default:
                printf("ERROR: Server sent an unknown type of response %d\n", rs.header.type);
                break;
            }
        }
    }

    return 0;
}

int client_deinit(ClientState *client) {
    printf("INFO: Closing client\n");
    close(client->sock_client);

    return 0;
}

void print_usage() {
    printf("USAGE: ./chat server|client host port\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage();
        return EXIT_SUCCESS;
    }

    char hostname[MAX_HOSTNAME_SIZE + 1] = DEFAULT_HOST;
    int port = DEFAULT_PORT;
    if (argc >= 3) {
        int hostname_len = strlen(argv[2]);
        if (hostname_len > MAX_HOSTNAME_SIZE) {
            printf("ERROR: Hostname too long\n");
            return EXIT_FAILURE;
        }

        memcpy(hostname, argv[2], hostname_len);
    }

    if (argc >= 4) {
        port = atoi(argv[3]);
    }

    if (strcmp(argv[1], "server") == 0) {
        ServerState server;
        int err = server_init(&server, hostname, port);
        if (err < 0) {
            printf("ERROR: server_init %d %d\n", err, errno);
            return EXIT_FAILURE;
        }

        err = server_start(&server);
        if (err < 0) {
            printf("ERROR: server_start %d %d\n", err, errno);
            return EXIT_FAILURE;
        }

        err = server_deinit(&server);
        if (err < 0) {
            printf("ERROR server_deinit %d %d\n", err, errno);
            return EXIT_FAILURE;
        }

        return EXIT_SUCCESS;
    }

    if (strcmp(argv[1], "client") == 0) {
        ClientState client;

        int err = client_init(&client, hostname, port);
        if (err < 0) {
            printf("ERROR: client_init %d %d\n", err, errno);
            return EXIT_FAILURE;
        }

        err = client_start(&client);
        if (err < 0) {
            printf("ERROR: client_start %d %d\n", err, errno);
            return EXIT_FAILURE;
        }

        err = client_deinit(&client);
        if (err < 0) {
            printf("ERROR client_deinit %d %d\n", err, errno);
            return EXIT_FAILURE;
        }

        return EXIT_SUCCESS;
    }
}
