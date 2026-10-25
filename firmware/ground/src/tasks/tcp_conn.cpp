// tcp_conn.cpp — ver tcp_conn.h
#include "tcp_conn.h"
#include <lwip/api.h>
#include <lwip/tcp.h>
#include <lwip/priv/tcpip_priv.h>
extern "C" {   // o sockets_priv.h declara o lwip_socket_dbg_get_socket sem extern "C"
#include <lwip/priv/sockets_priv.h>
}

namespace {

enum class Op : uint8_t { Reset, InFlight };

struct ConnCall {
    tcpip_api_call_data base;   // primeiro membro: o tcpip_api_call recebe o endereço dele
    netconn *conn;
    Op op;
    int result;
};

// Roda na thread do TCP/IP
err_t inTcpip(tcpip_api_call_data *d) {
    ConnCall *c = reinterpret_cast<ConnCall *>(d);
    tcp_pcb *pcb = NETCONNTYPE_GROUP(netconn_type(c->conn)) == NETCONN_TCP ? c->conn->pcb.tcp : nullptr;
    if (pcb == nullptr) {
        c->result = -1;
        return ERR_OK;
    }
    if (c->op == Op::Reset) {
        tcp_abort(pcb);   // manda RST, libera o pcb e avisa a netconn (erro ERR_ABRT)
        c->result = 1;
    } else {
        c->result = (int)TCP_SND_BUF - (int)tcp_sndbuf(pcb);
    }
    return ERR_OK;
}

int call(int fd, Op op) {
    if (fd < 0) return -1;
    lwip_sock *s = lwip_socket_dbg_get_socket(fd);   // o socket é da web_task, que não o fecha durante a chamada
    if (s == nullptr || s->conn == nullptr) return -1;
    ConnCall c = {};
    c.conn = s->conn;
    c.op = op;
    c.result = -1;
    tcpip_api_call(inTcpip, &c.base);
    return c.result;
}

}   // namespace

bool tcpReset(int fd) { return call(fd, Op::Reset) > 0; }

int tcpInFlight(int fd) { return call(fd, Op::InFlight); }
