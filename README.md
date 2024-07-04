# A very simple terminal chat client
## Installation 
```
$ gcc main.c -o chat
```

## Usage
### Starting a server

```
$ ./chat server {host (default 127.0.0.1)} {port (default 8080)}
$ // Example
$ ./chat server 192.168.1.101 1234
```

### Starting a client

```
$ ./chat client {host (default 127.0.0.1)} {port (default 8080)}
$ // Example
$ ./chat client 192.168.1.101 1234
> Write your name:
> rndm13
> You can now write messages to chat:
> SERVER INFO: user rndm13 has joined the chat room
> Hello!
> rndm13: Hello!
```
