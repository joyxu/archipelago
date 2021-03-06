/*
 * Copyright (C) 2014 GRNET S.A.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <cstdio>
#include <cstring>
#include <iostream>
#include <list>
#include <map>
#include <utility>
#include <algorithm>
#include <cstdlib>
#include <cerrno>
#include <functional>

#include <arpa/inet.h>
#include <sys/eventfd.h>
#include <pthread.h>
#include <unistd.h>
#include <getopt.h>

#include "poold/poold.hh"
#include "poold/system.hh"
#include "poold/sighandler.hh"

using namespace std;

namespace archipelago {

Poold::Poold(const int& startrange, const int& endrange,
        const string& uendpoint)
    : Logger("logging.conf", "Poold")
{
    initialize(startrange, endrange, uendpoint);
}

Poold::Poold(const int& startrange, const int& endrange,
        const string& uendpoint, const string& logconf)
    : Logger(logconf, "Poold")
{
    initialize(startrange, endrange, uendpoint);
}

void Poold::initialize(const int& start, const int& end,
        const string& uendpoint)
{
    bRunning = true;
    endpoint = uendpoint;
    startrange = start;
    endrange  = end;
    for (int i = startrange; i < endrange + 1; i++) {
        port_pool.push_back(i);
    }
    pthread_mutex_init(&mutex, NULL);
}

Socket *Poold::find_socket(int fd)
{
    map<Socket*, int>::iterator it;
    for (it = socket_connection_state.begin();
            it!= socket_connection_state.end(); ++it) {
        if (it->first->get_fd() == fd) {
            break;
        }
    }
    return it->first;
}

void Poold::set_socket_pollin(Socket& socket)
{
    if (!epoll.reset_socket_pollout(socket)) {
        logerror("epoll.reset_socket_pollout error");
    }
    if (!epoll.set_socket_pollin(socket)) {
        logerror("epoll.set_socket_pollin error");
    }
}

void Poold::set_socket_pollout(Socket& socket)
{
    if (!epoll.reset_socket_pollin(socket)) {
        logerror("epoll.reset_socket_pollin error");
    }
    if (!epoll.set_socket_pollout(socket)) {
        logerror("epoll.set_socket_pollout error");
    }
}

void Poold::server()
{
    if (!srvsock.create()) {
        logfatal("Could not create server socket. Aborting...");
        exit(EXIT_FAILURE);
    }

    if (!srvsock.bind(endpoint)) {
        logfatal("Could not bind to endpoint. Aborting...");
        exit(EXIT_FAILURE);
    }

    if (!srvsock.listen(5)) {
        logfatal("Could not listen to socket. Aborting...");
        exit(EXIT_FAILURE);
    }

    if (!epoll.add_socket(srvsock, EPOLLIN)) {
        logfatal("Could not add server socket for polling (epoll). Aborting...");
        exit(EXIT_FAILURE);
    }

    evfd = eventfd(0, EFD_NONBLOCK);
    if (!epoll.add_fd(evfd, EPOLLIN | EPOLLET)) {
        logfatal("Could not add eventfd file descriptor for polling (epoll). Aborting...");
        exit(EXIT_FAILURE);
    }

    socket_connection_state[&srvsock] = NONE;
}

void Poold::create_new_connection(Socket& socket)
{
    if (socket.get_fd() == -1) {
        logfatal("Socket file descriptor error. Aborting...");
        exit(EXIT_FAILURE);
    }
    socket.setnonblocking(true);
    epoll.add_socket(socket, EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR);
    socket_connection_state[&socket] = NONE;
    logdebug("Accepted new connection");
}

void Poold::clear_connection(Socket& socket)
{
    epoll.rm_socket(socket);
    list<int>::iterator i;
    list<int> L = socket_connection_ports[&socket];
    logdebug("Clearing connection");

    pthread_mutex_lock(&mutex);
    for ( i = L.begin(); i != L.end(); i++) {
        port_pool.push_front(*i);
    }

    socket_connection_state.erase(&socket);
    socket_connection_ports[&socket].clear();
    socket_connection_ports.erase(&socket);
    pthread_mutex_unlock(&mutex);
}

poolmsg_t *Poold::recv_msg(const Socket& socket)
{
    unsigned int buffer[2];
    poolmsg_t *msg;

    logdebug("Receiving new message.");
    if (!socket.read(&buffer, sizeof(buffer))) {
        logerror("Socket read error.");
    }
    msg = reinterpret_cast<poolmsg_t *>(calloc(1, sizeof(poolmsg_t)));
    msg->type = ntohl(buffer[0]);
    msg->port = ntohl(buffer[1]);
    return msg;
}

int Poold::send_msg(const Socket& socket, int port)
{
    const int buffer[1] = {port};
    logdebug("Sending port to client.");

    int n = socket.write(buffer, sizeof(buffer));
    if (n < 0) {
        logerror("Socket write error.");
    }
    return n;
}

int Poold::get_new_port(Socket& socket)
{
    if (port_pool.empty()) {
        logdebug("Port pool is empty.");
        return -1;
    }
    pthread_mutex_lock(&mutex);
    int port = port_pool.front();
    port_pool.pop_front();
    socket_connection_ports[&socket].push_front(port);
    pthread_mutex_unlock(&mutex);
    return port;
}

void Poold::handle_request(Socket& socket, poolmsg_t *msg)
{
    list<int>::iterator i;
    list<int> L = socket_connection_ports[&socket];
    logdebug("Handle request.");

    if (msg->type == GET_PORT) {
        socket_connection_state[&socket] = REPLY_PORT;
    } else if (msg->type == LEAVE_PORT) {
        if (find(L.begin(), L.end(), msg->port) != L.end()) {
            socket_connection_ports[&socket].remove(msg->port);
            pthread_mutex_lock(&mutex);
            port_pool.push_front(msg->port);
            pthread_mutex_unlock(&mutex);
            socket_connection_state[&socket] = REPLY_LEAVE_PORT_SUCCESS;
        } else {
            socket_connection_state[&socket] = REPLY_LEAVE_PORT_FAIL;
        }
    } else if (msg->type == LEAVE_ALL_PORTS) {
        for ( i = L.begin(); i != L.end(); i++) {
            socket_connection_ports[&socket].remove(*i);
            pthread_mutex_lock(&mutex);
            port_pool.push_front(*i);
            pthread_mutex_unlock(&mutex);
        }
        socket_connection_state[&socket] = REPLY_LEAVE_ALL_PORTS;
    }
    Poold::set_socket_pollout(socket);
    free(msg);
}

void Poold::serve_forever()
{
    poolmsg_t *msg;
    while (Poold::bRunning) {
        int nfds = epoll.wait(events, 20, -1);
        if (nfds == -1 && errno != EINTR) {
            logfatal("epoll.wait fatal error. Aborting...");
            exit(EXIT_FAILURE);
        }
        if (!Poold::bRunning) {
            break; //Cleanup
        }

        for (int n = 0; n < nfds; n++) {
            int epfd = events[n].data.fd;
            if (epfd == srvsock.get_fd()) {
                Socket *clientsock = new Socket();
                if (!srvsock.accept(*clientsock)) {
                    logfatal("Could not accept socket. Aborting...");
                    exit(EXIT_FAILURE);
                }
                Poold::create_new_connection(*clientsock);
            } else if (epfd == evfd) {
                /* Exit loop */
                return;
            } else if (events[n].events & EPOLLRDHUP ||
                            events[n].events & EPOLLHUP ||
                            events[n].events & EPOLLERR) {
                Socket *clientsock = Poold::find_socket(epfd);
                Poold::clear_connection(*clientsock);
                delete clientsock;
                ::close(epfd);
            } else if (events[n].events & EPOLLIN) {
                Socket *clientsock = Poold::find_socket(epfd);
                msg = Poold::recv_msg(*clientsock);
                Poold::handle_request(*clientsock, msg);
            } else if (events[n].events & EPOLLOUT) {
                Socket *clientsock = Poold::find_socket(epfd);
                switch (socket_connection_state[clientsock]) {
                case REPLY_PORT:
                    Poold::send_msg(*clientsock, get_new_port(*clientsock));
                    break;
                case REPLY_LEAVE_PORT_SUCCESS:
                    Poold::send_msg(*clientsock, 1);
                    break;
                case REPLY_LEAVE_PORT_FAIL:
                    Poold::send_msg(*clientsock, 0);
                    break;
                case REPLY_LEAVE_ALL_PORTS:
                    Poold::send_msg(*clientsock, 1);
                    break;
                default:
                    Poold::send_msg(*clientsock, 0);
                    logerror("Unknown state.");
                }
                socket_connection_state[clientsock] = NONE;
                Poold::set_socket_pollin(*clientsock);
            }
        }
    }
}

void Poold::run()
{
    int rv = pthread_create(&th, NULL, poold_helper, static_cast<void*>(this));
    if (rv != 0) {
        logfatal("Error in thread creation. Aborting...");
        exit(EXIT_FAILURE);
    }
}

void Poold::close()
{
    Poold::bRunning = false;
    eventfd_write(evfd, 1);
    pthread_join(th, NULL);
    loginfo("Cleanup.");
    unlink(endpoint.c_str());
}

}

void print_usage(int argc, char **argv, string pidfile, string socketpath)
{
    std::cout << "Usage: " << argv[0] << " [options]\n"
        "\nOptions:\n"
        "-h, --help\t\tprint this help message\n"
        "-s, --start\t\tset start of the pool range (default: 1)\n"
        "-e, --end\t\tset end of the pool range (default: 100)\n"
        "-p, --socketpath\tset socket path (default: '"<< socketpath << "')\n"
        "-c, --logconfig\t\tset logging configuration file (default: none)\n"
        "-i, --pidfile\t\tset pidfile (default: '"<< pidfile << "')\n"
        "-u, --user\t\tset real EUID\n"
        "-g, --group\t\tset real EGID\n"
        "-m, --umask\t\tset umask (default: 0007)\n"
        "-d, --daemonize\t\tdaemonize (default: no)\n"
        "\n";
}

int main(int argc, char **argv)
{
    int option = 0;
    int uid = -1;
    int gid = -1;
    bool daemonize = false;
    int startpoolrange = 1;
    int endpoolrange = 100;
    mode_t mask = 0007;
    sigset_t tmpsigset;
#ifdef POOLD_SOCKET_PATH
    string socketpath (POOLD_SOCKET_PATH);
#else
    string socketpath ("poold.socket");
#endif
    string logconffile;
#ifdef POOLD_PIDFILE
    string pidfile (POOLD_PIDFILE);
#else
    string pidfile ("poold.pid");
#endif

    static struct option poold_long_opts[] = {
        {"help", no_argument, 0, 'h'},
        {"start", required_argument, 0, 's'},
        {"end", required_argument, 0, 'e'},
        {"socketpath", required_argument, 0, 'p'},
        {"logconfig", required_argument, 0, 'c'},
        {"pidfile", required_argument, 0, 'i'},
        {"user", required_argument, 0, 'u'},
        {"group", required_argument, 0, 'g'},
        {"umask", required_argument, 0, 'm'},
        {"daemonize", no_argument, 0, 'd'},
        {0, 0, 0, 0}
    };

    int long_opts_index = 0;
    while ((option = getopt_long(argc, argv, "hds:e:p:u:g:i:m:c:",
                    poold_long_opts, &long_opts_index)) != -1) {
        switch (option) {
        case 's':
            startpoolrange= atoi(optarg);
            break;
        case 'e':
            endpoolrange= atoi(optarg);
            break;
        case 'p':
            socketpath.assign(optarg, strlen(optarg));
            break;
        case 'c':
            logconffile.assign(optarg, strlen(optarg));
            break;
        case 'd':
            daemonize = true;
            break;
        case 'u':
            uid = atoi(optarg);
            break;
        case 'g':
            gid = atoi(optarg);
            break;
        case 'i':
            pidfile = pidfile.assign(optarg, strlen(optarg));
            break;
        case 'm':
            mask = atol(optarg);
            break;
        case 'h':
            print_usage(argc, argv, pidfile, socketpath);
            exit(EXIT_SUCCESS);
        default:
            print_usage(argc, argv, pidfile, socketpath);
            exit(EXIT_FAILURE);
        }
    }

    archipelago::System system = archipelago::System(logconffile);

    if (system.set_system(daemonize, uid, gid, mask, pidfile) < 0) {
        system.logerror("Cannot set application settings. Aborting...");
        exit(EXIT_FAILURE);
    }

    archipelago::Poold pool = archipelago::Poold(startpoolrange, endpoolrange,
            socketpath, logconffile);
    pool.server();
    pool.loginfo("Running server.");
    pool.run();

    try {
        archipelago::SigHandler sigh;
        sigh.setupSignalHandlers();
        pool.loginfo("Setting up signal handlers.");

        (void) sigemptyset(&tmpsigset);
        while (!sigh.gotExitSignal()) {
            sigsuspend(&tmpsigset);
        }
    } catch (archipelago::SigException& e) {
        pool.logfatal("Signal Handler Exception: " + std::string(e.what()));
    }
    pool.close();
    system.remove_pid(pidfile);
    pool.loginfo("Closing server.");
    return 0;
}
