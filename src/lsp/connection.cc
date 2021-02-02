#include "connection.h"
#include "connection_handler.h"
#include "messages.h"

#include <QTcpSocket>
#include <QTextStream>
#include <QSocketNotifier>
#include <QFile>

#include <iostream>
#include <list>
#include <memory>


// Print raw message traffic (in & out) to stdout
#define DEBUG_MESSAGETRAFFIC

// Print remotely sent log messages locally
#define LOCAL_LOG_MESSAGES



Connection::Connection(ConnectionHandler *handler, std::unique_ptr<project> &&project,
    std::unique_ptr<QTextStream> &&in_stream, std::unique_ptr<QTextStream> &&out_stream) :
        handler(handler),
        in_stream(std::move(in_stream)),
        out_stream(std::move(out_stream)),
        active_project(std::move(project))
{
}



static bool is_strip_empty(const QString &data) {
    for(const auto &b : data.toStdString()) {
        if (!std::isspace(b)) {
            return false;
        }
    }
    return true;
}

static std::pair<std::string, std::string> read_header_line(QString &data) {
    std::string str = data.toStdString();
     // Separate the header
    size_t sep_pos = str.find(": ");
    if (sep_pos == std::string::npos) {
        return {"", "no sep"};
    }
    size_t end_pos = str.find("\r\n");

    // +2 because of ": " as header separator
    return {str.substr(0, sep_pos), str.substr(sep_pos + 2, end_pos)};
}

void Connection::read_header() {
    while (!this->in_stream->atEnd()) {
        QString pending_data = this->in_stream->readLine();
        if(is_strip_empty(pending_data)) {
            this->packet_state = PACKET_EXPECT::BODY;
            return;
        }

        auto field = read_header_line(pending_data);

        // EXTEND: List Accepted header options here
        if (field.first == "Content-Length") {
            size_t l = std::atoi(field.second.c_str());
            //if (l < 1024 * 1024 * 5)  { // TODO: Try to trust a remote network connection?!?!
            if (l > 0) {
                this->header.content_length = l;
            }
        } else if (field.first == "Content-Type") {
            if (field.second != "application/vscode-jsonrpc; charset=utf-8") {
                std::cerr << "unexpected content type " << field.second << "\n";
            }
        } else {
            this->warn(std::string("Unknown header field ") + field.first);
        }
    }
}

void Connection::read_body() {
    if (header.content_length == 0) {
        std::cerr << "No Content-Length given\n";
        packet_state = PACKET_EXPECT::HEADER;
        return;
    }

    QString pending_data = this->in_stream->read(this->header.content_length);
#ifdef DEBUG_MESSAGETRAFFIC
    std::cout << "RECEIVED: [" << pending_data.size() << "]: " << pending_data.toStdString() << "\n";
    std::cout << "\n";
#endif

    this->handler->handle_message(pending_data, this);

    // Reset to receive header
    packet_state = PACKET_EXPECT::HEADER;
    this->header = connection_header();
}

void Connection::onReadyRead() {
    switch(packet_state) {
    case PACKET_EXPECT::HEADER:
        this->read_header();
        break;
    case PACKET_EXPECT::BODY:
        this->read_body();
        break;
    }
}


void Connection::clean_pending_messages(const std::chrono::system_clock::duration &max_age) {
    auto now = std::chrono::system_clock::now();

    // There is no support for remove_if in associative containers. sometimes I love you C++.
    // If it were, we could say something like
    //pending_messages.erase(std::remove_if(pending_messages.begin(), pending_messages.end(), [&](const std::pair<int, pending_message> &msg) {
    //    return (now - msg.second.pending_since) > max_age;
    //}));
    int cnt = 0;
    for(auto it = pending_messages.begin(); it != pending_messages.end();){
        if ((now - it->second.pending_since) > max_age) {
            it = pending_messages.erase(it); // previously this was something like m_map.erase(it++);
            cnt ++;
        } else {
            ++it;
        }
    }

    if (cnt > 0) {
        std::cout << "Cleared " << cnt << "stale messages with a missing response\n";
    }
}

void Connection::default_reporting_message_handler(const ResponseMessage &msg, Connection *, project *) {
    std::cout << "Unhandled message (TODO add more info)\n" << msg.id.value_int << "\n";
}

void Connection::no_response_expected(const ResponseMessage &msg, Connection *, project *) {
    if (msg.error) {
        std::cout << "The Request with ID " << msg.id.value_int << " has failed with error code " << static_cast<int>(msg.error->code) << ": \n"
            << msg.error->message << "\n";
    }
}

void Connection::handle_pending_response(const ResponseMessage &msg) {
    if (msg.id.type != RequestId::INT) {
        std::cout << "Received data without a handle-able ID " << msg.id.value_str << "\n";
        return;
    }

    auto it = pending_messages.find(msg.id.value_int);
    if (it != pending_messages.end()) {
        it->second.callback(msg, this, this->active_project.get());
        pending_messages.erase(it);
    }
}

void Connection::send(const QByteArray &data) {
    // Send headers
    {
        QByteArray headerbuf;
        headerbuf.append("Content-Length: ")
            .append(QString::number(data.size()).toUtf8())
            .append("\r\n\r\n");
        *this->out_stream << headerbuf;
    }

    // Send payload
    *this->out_stream << data;
#ifdef DEBUG_MESSAGETRAFFIC
    std::cout << "SENDING: [" << data.size() << "]: "
        << data.data();
    std::cout << "\n";
#endif
}

void Connection::send(ResponseMessage &msg, const RequestId &id) {
    if (!msg.id.is_set())
        msg.id = id;

    QByteArray responsebuffer;
    decode_env env(storage_direction::WRITE);
    env.store(&responsebuffer, msg);

    this->send(responsebuffer);
}

void Connection::send(RequestMessage &msg, const std::string &method, const RequestId &id, request_callback_t callback) {
    if (!msg.id.is_set()) {
        msg.id = id;
    }
    if (msg.id.type == RequestId::AUTO_INCREMENT) {
        msg.id.type = RequestId::INT;
        msg.id.value_int = this->next_request_id;
        this->next_request_id ++;
    }

    if (msg.method.empty()) {
        msg.method = method;
    }

    if (msg.id.is_set()) {
        pending_messages.emplace(std::make_pair(msg.id.value_int, pending_message(callback)));
    }

    QByteArray responsebuffer;
    decode_env env(storage_direction::WRITE);
    env.store(&responsebuffer, msg);

    this->send(responsebuffer);
}


void Connection::send(ResponseResult &result, const RequestId &id) {
    ResponseMessage msg(result);
    this->send(msg, id);
}

void Connection::send(ResponseResult &&result, const RequestId &id) {
    ResponseMessage msg(result);
    this->send(msg, id);
}

void Connection::send(ResponseError &error, const RequestId &id) {
    ResponseMessage msg(nullptr);
    msg.error = error;
    this->send(msg, id);
}

void Connection::send(ResponseError &&error, const RequestId &id) {
    ResponseMessage msg(nullptr);
    msg.error = error;
    this->send(msg, id);
}

void Connection::log(MessageType type, const std::string &message) {
#ifdef LOCAL_LOG_MESSAGES
    std::cout << "LOG " << message << "\n";
#endif

    ShowMessageParams msg;
    msg.type = type;
    msg.message = message;

    this->send(msg, "window/showMessage", {}, &Connection::no_response_expected); // No ID set
}


TCPLSPConnection::TCPLSPConnection(ConnectionHandler *handler, QTcpSocket *client, std::unique_ptr<project> &&project) :
        Connection(handler, std::move(project),
            std::make_unique<QTextStream>(client),
            std::make_unique<QTextStream>(client)),
        socket(client)
{
   connect(socket, SIGNAL(readyRead()), this, SLOT(onReadyRead()));
}

void TCPLSPConnection::close() {
    this->socket->close();
    assert(!this->socket->isOpen());
}

bool TCPLSPConnection::is_done() {
    return !this->socket->isOpen();
}

std::string TCPLSPConnection::peerName() {
    return this->socket->peerName().toStdString() + ":" + std::to_string(this->socket->peerPort());
}


StdioLSPConnection::StdioLSPConnection(ConnectionHandler *handler, std::unique_ptr<project> &&project) :
    Connection(handler, std::move(project),
        std::make_unique<QTextStream>(stdin, QIODevice::ReadOnly),
        std::make_unique<QTextStream>(stdout, QIODevice::WriteOnly)),
    notifier(std::make_unique<QSocketNotifier>(STDIN_FILENO, QSocketNotifier::Read, this))
{
    connect(notifier.get(), SIGNAL(activated()), this, SLOT(onReadyRead()));
}

StdioLSPConnection::~StdioLSPConnection() {}