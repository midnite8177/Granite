#include "fs-netfs.hpp"
#include "../path.hpp"
#include "util.hpp"
#include <queue>
#include <assert.h>

using namespace std;

namespace Granite
{
struct FSNotifyCommand : LooperHandler
{
	FSNotifyCommand(const string &protocol, unique_ptr<Socket> socket)
		: LooperHandler(move(socket))
	{
		reply_queue.emplace();
		auto &reply = reply_queue.back();
		reply.builder.add_u32(NETFS_NOTIFICATION);
		reply.builder.add_u32(NETFS_BEGIN_CHUNK_REQUEST);
		reply.builder.add_string(protocol);
		reply.writer.start(reply.builder.get_buffer());

		result_reply.begin(4 * sizeof(uint32_t));
		command_reader.start(result_reply.get_buffer());

		state = NotificationLoop;
	}

	~FSNotifyCommand()
	{
		LOGE("Destroying FSNotifyCommand!\n");
	}

	void set_notify_cb(function<void (const FileNotifyInfo &)> func)
	{
		notify_cb = move(func);
	}

	void push_register_notification(const string &path, promise<FileNotifyHandle> result)
	{
		if (reply_queue.empty() && socket->get_parent_looper())
			socket->get_parent_looper()->modify_handler(EVENT_IN | EVENT_OUT, *this);

		reply_queue.emplace();
		auto &reply = reply_queue.back();
		reply.builder.add_u32(NETFS_REGISTER_NOTIFICATION);
		reply.builder.add_string(path);
		reply.writer.start(reply.builder.get_buffer());

		replies.push(move(result));
	}

	void push_unregister_notification(FileNotifyHandle handler, promise<FileNotifyHandle> result)
	{
		if (reply_queue.empty() && socket->get_parent_looper())
			socket->get_parent_looper()->modify_handler(EVENT_IN | EVENT_OUT, *this);

		reply_queue.emplace();
		auto &reply = reply_queue.back();
		reply.builder.add_u32(NETFS_UNREGISTER_NOTIFICATION);
		reply.builder.add_u64(8);
		reply.builder.add_u64(uint64_t(handler));
		reply.writer.start(reply.builder.get_buffer());
		replies.push(move(result));
	}

	void modify_looper(Looper &looper)
	{
		uint32_t mask = reply_queue.empty() ? EVENT_IN : (EVENT_IN | EVENT_OUT);
		looper.modify_handler(mask, *this);
	}

	bool read_reply_data(Looper &looper)
	{
		auto ret = command_reader.process(*socket);
		if (command_reader.complete())
		{
			if (last_cmd == NETFS_BEGIN_CHUNK_NOTIFICATION)
			{
				FileNotifyInfo info;
				info.path = result_reply.read_string();
				info.handle = FileNotifyHandle(result_reply.read_u64());
				auto type = result_reply.read_u32();
				switch (type)
				{
				case NETFS_FILE_CHANGED:
					info.type = FileNotifyType::FileChanged;
					break;
				case NETFS_FILE_DELETED:
					info.type = FileNotifyType::FileDeleted;
					break;
				case NETFS_FILE_CREATED:
					info.type = FileNotifyType::FileCreated;
					break;
				}

				notify_cb(info);
				result_reply.begin(4 * sizeof(uint32_t));
				command_reader.start(result_reply.get_buffer());
				modify_looper(looper);
				state = NotificationLoop;
				return true;
			}
			else if (last_cmd == NETFS_BEGIN_CHUNK_REPLY)
			{
				auto handle = int(result_reply.read_u64());

				try
				{
					replies.front().set_value(handle);
				}
				catch (...)
				{
				}

				assert(!replies.empty());
				replies.pop();

				result_reply.begin(4 * sizeof(uint32_t));
				command_reader.start(result_reply.get_buffer());
				modify_looper(looper);
				state = NotificationLoop;
				return true;
			}
			else
				return false;
		}

		return (ret > 0) || (ret == Socket::ErrorWouldBlock);
	}

	bool notification_loop(Looper &looper, EventFlags flags)
	{
		if (flags & EVENT_OUT)
		{
			if (reply_queue.empty())
			{
				looper.modify_handler(EVENT_IN, *this);
				return true;
			}

			auto ret = reply_queue.front().writer.process(*socket);
			if (reply_queue.front().writer.complete())
				reply_queue.pop();

			if (reply_queue.empty())
			{
				looper.modify_handler(EVENT_IN, *this);
				return true;
			}
			else
				return (ret > 0) || (ret == Socket::ErrorWouldBlock);
		}

		if (flags & EVENT_IN)
		{
			auto ret = command_reader.process(*socket);
			if (command_reader.complete())
			{
				auto cmd = result_reply.read_u32();
				if (cmd == NETFS_BEGIN_CHUNK_NOTIFICATION || cmd == NETFS_BEGIN_CHUNK_REPLY)
				{
					if (result_reply.read_u32() != NETFS_ERROR_OK)
						return false;

					last_cmd = cmd;
					auto size = result_reply.read_u64();
					result_reply.begin(size);
					command_reader.start(result_reply.get_buffer());
					state = ReadReplyData;
					looper.modify_handler(EVENT_IN, *this);
					return true;
				}
				else
					return false;
			}

			return (ret > 0) || (ret == Socket::ErrorWouldBlock);
		}

		return true;
	}

	bool handle(Looper &looper, EventFlags flags) override
	{
		if (state == ReadReplyData)
			return read_reply_data(looper);
		else if (state == NotificationLoop)
			return notification_loop(looper, flags);
		else
			return false;
	}

	enum State
	{
		ReadReplyData,
		NotificationLoop
	};

	State state = NotificationLoop;
	SocketReader command_reader;
	ReplyBuilder result_reply;
	uint32_t last_cmd = 0;

	struct NotificationReply
	{
		SocketWriter writer;
		ReplyBuilder builder;
	};
	queue<NotificationReply> reply_queue;
	queue<promise<FileNotifyHandle>> replies;
	function<void (const FileNotifyInfo &info)> notify_cb;
};

struct FSReadCommand : LooperHandler
{
	virtual ~FSReadCommand() = default;

	FSReadCommand(const string &path, NetFSCommand command, unique_ptr<Socket> socket)
		: LooperHandler(move(socket))
	{
		reply_builder.begin();
		reply_builder.add_u32(command);
		reply_builder.add_u32(NETFS_BEGIN_CHUNK_REQUEST);
		reply_builder.add_string(path);
		command_writer.start(reply_builder.get_buffer());
		state = WriteCommand;
	}

	bool write_command(Looper &looper)
	{
		auto ret = command_writer.process(*socket);
		if (command_writer.complete())
		{
			state = ReadReplySize;
			reply_builder.begin(4 * sizeof(uint32_t));
			command_reader.start(reply_builder.get_buffer());
			looper.modify_handler(EVENT_IN, *this);
			return true;
		}

		return (ret > 0) || (ret == Socket::ErrorWouldBlock);
	}

	bool read_reply_size(Looper &)
	{
		auto ret = command_reader.process(*socket);
		if (command_reader.complete())
		{
			if (reply_builder.read_u32() != NETFS_BEGIN_CHUNK_REPLY)
				return false;

			if (reply_builder.read_u32() != NETFS_ERROR_OK)
				return false;

			uint64_t reply_size = reply_builder.read_u64();
			if (reply_size == 0)
				return false;

			reply_builder.begin(reply_size);
			command_reader.start(reply_builder.get_buffer());
			state = ReadReply;
			return true;
		}

		return (ret > 0) || (ret == Socket::ErrorWouldBlock);
	}

	bool read_reply(Looper &)
	{
		auto ret = command_reader.process(*socket);
		if (command_reader.complete())
		{
			parse_reply();
			return false;
		}

		return (ret > 0) || (ret == Socket::ErrorWouldBlock);
	}

	bool handle(Looper &looper, EventFlags) override
	{
		if (state == WriteCommand)
			return write_command(looper);
		else if (state == ReadReplySize)
			return read_reply_size(looper);
		else if (state == ReadReply)
			return read_reply(looper);
		else
			return false;
	}

	enum State
	{
		WriteCommand,
		ReadReplySize,
		ReadReply
	};
	State state = WriteCommand;
	SocketReader command_reader;
	SocketWriter command_writer;
	ReplyBuilder reply_builder;

	virtual void parse_reply() = 0;
};

struct FSReader : FSReadCommand
{
	FSReader(const string &path, unique_ptr<Socket> socket)
		: FSReadCommand(path, NETFS_READ_FILE, move(socket))
	{
	}

	~FSReader()
	{
		if (!got_reply)
			result.set_exception(make_exception_ptr(runtime_error("file read")));
	}

	void parse_reply() override
	{
		got_reply = true;
		try
		{
			result.set_value(reply_builder.consume_buffer());
		}
		catch (...)
		{
		}
	}

	promise<vector<uint8_t>> result;
	bool got_reply = false;
};

struct FSList : FSReadCommand
{
	FSList(const string &path, unique_ptr<Socket> socket)
		: FSReadCommand(path, NETFS_LIST, move(socket))
	{
	}

	~FSList()
	{
		if (!got_reply)
			result.set_exception(make_exception_ptr(runtime_error("List failed")));
	}

	void parse_reply() override
	{
		uint32_t entries = reply_builder.read_u32();
		vector<ListEntry> list;
		for (uint32_t i = 0; i < entries; i++)
		{
			auto path = reply_builder.read_string();
			auto type = reply_builder.read_u32();

			switch (type)
			{
			case NETFS_FILE_TYPE_PLAIN:
				list.push_back({ move(path), PathType::File });
				break;
			case NETFS_FILE_TYPE_DIRECTORY:
				list.push_back({ move(path), PathType::Directory });
				break;
			case NETFS_FILE_TYPE_SPECIAL:
				list.push_back({ move(path), PathType::Special });
				break;
			}
		}

		got_reply = true;
		try
		{
			result.set_value(move(list));
		}
		catch (...)
		{
		}
	}

	promise<vector<ListEntry>> result;
	bool got_reply = false;
};

struct FSStat : FSReadCommand
{
	FSStat(const string &path, unique_ptr<Socket> socket)
		: FSReadCommand(path, NETFS_STAT, move(socket))
	{
	}

	~FSStat()
	{
		// Throw exception instead in calling thread.
		if (!got_reply)
			result.set_exception(make_exception_ptr(runtime_error("Failed stat")));
	}

	void parse_reply() override
	{
		uint64_t size = reply_builder.read_u64();
		uint32_t type = reply_builder.read_u32();
		FileStat s;
		s.size = size;

		switch (type)
		{
		case NETFS_FILE_TYPE_PLAIN:
			s.type = PathType::File;
			break;
		case NETFS_FILE_TYPE_DIRECTORY:
			s.type = PathType::Directory;
			break;
		case NETFS_FILE_TYPE_SPECIAL:
			s.type = PathType::Special;
			break;
		}

		got_reply = true;
		try
		{
			result.set_value(s);
		}
		catch (...)
		{
		}
	}

	std::promise<FileStat> result;
	bool got_reply = false;
};

struct FSWriteCommand : LooperHandler
{
	FSWriteCommand(const string &path, const vector<uint8_t> &buffer, unique_ptr<Socket> socket)
		: LooperHandler(move(socket))
	{
		target_size = buffer.size();

		reply_builder.begin();
		result_reply.begin(4 * sizeof(uint32_t));

		reply_builder.add_u32(NETFS_WRITE_FILE);
		reply_builder.add_u32(NETFS_BEGIN_CHUNK_REQUEST);
		reply_builder.add_string(path);
		reply_builder.add_u32(NETFS_BEGIN_CHUNK_REQUEST);
		reply_builder.add_u64(buffer.size());
		reply_builder.add_buffer(buffer);
		command_writer.start(reply_builder.get_buffer());
		command_reader.start(result_reply.get_buffer());
		state = WriteCommand;
	}

	bool write_command(Looper &looper, EventFlags flags)
	{
		if (flags & EVENT_IN)
		{
			auto ret = command_reader.process(*socket);
			// Received message before we completed the write, must be an error.
			if (command_reader.complete())
				return false;

			return (ret > 0) || (ret == Socket::ErrorWouldBlock);
		}
		else if (flags & EVENT_OUT)
		{
			auto ret = command_writer.process(*socket);
			if (command_writer.complete())
			{
				// Done writing, wait for reply.
				looper.modify_handler(EVENT_IN, *this);
				state = ReadReply;
			}

			return (ret > 0) || (ret == Socket::ErrorWouldBlock);
		}

		return true;
	}

	~FSWriteCommand()
	{
		if (!got_reply)
			result.set_exception(make_exception_ptr(runtime_error("Failed write")));
	}

	bool read_reply(Looper &)
	{
		auto ret = command_reader.process(*socket);
		if (command_reader.complete())
		{
			if (result_reply.read_u32() != NETFS_BEGIN_CHUNK_REPLY)
				return false;
			if (result_reply.read_u32() != NETFS_ERROR_OK)
				return false;
			if (result_reply.read_u64() != target_size)
				return false;

			got_reply = true;
			try
			{
				result.set_value(NETFS_ERROR_OK);
			}
			catch (...)
			{
			}
			return false;
		}

		return (ret > 0) || (ret == Socket::ErrorWouldBlock);
	}

	bool handle(Looper &looper, EventFlags flags) override
	{
		if (state == WriteCommand)
			return write_command(looper, flags);
		else if (state == ReadReply)
			return read_reply(looper);
		else
			return false;
	}

	enum State
	{
		WriteCommand,
		ReadReply
	};

	State state = WriteCommand;
	SocketReader command_reader;
	SocketWriter command_writer;
	ReplyBuilder reply_builder;
	ReplyBuilder result_reply;
	size_t target_size = 0;

	promise<NetFSError> result;
	bool got_reply = false;
};

NetworkFilesystem::NetworkFilesystem()
{
	looper_thread = thread(&NetworkFilesystem::looper_entry, this);
}

void NetworkFilesystem::looper_entry()
{
	while (looper.wait_idle(-1) >= 0);
}

void NetworkFilesystem::setup_notification()
{
	auto socket = Socket::connect("127.0.0.1", 7070);
	if (!socket)
		return;
	notify = new FSNotifyCommand(protocol, move(socket));
	notify->set_notify_cb([this](const FileNotifyInfo &info) {
		signal_notification(info);
	});

	// Move capture would be nice ...
	looper.run_in_looper([this]() {
		looper.register_handler(EVENT_OUT, unique_ptr<FSNotifyCommand>(notify));
	});
}

void NetworkFilesystem::uninstall_notification(FileNotifyHandle handle)
{
	if (!notify)
		setup_notification();
	if (!notify)
		return;

	auto itr = handlers.find(handle);
	if (itr == end(handlers))
		return;
	handlers.erase(itr);

	auto *value = new promise<FileNotifyHandle>;
	auto result = value->get_future();
	looper.run_in_looper([this, value, handle]() {
		notify->push_unregister_notification(handle, move(*value));
		delete value;
	});

	try
	{
		result.wait();
	}
	catch (...)
	{
	}
}

void NetworkFilesystem::signal_notification(const FileNotifyInfo &info)
{
	lock_guard<mutex> holder{lock};
	pending.push_back(info);
}

void NetworkFilesystem::poll_notifications()
{
	lock_guard<mutex> holder{lock};
	for (auto &notification : pending)
	{
		auto &func = handlers[notification.handle];
		if (func)
			func(notification);
	}
	pending.clear();
}

FileNotifyHandle NetworkFilesystem::install_notification(const std::string &path,
                                                         std::function<void(const FileNotifyInfo &)> func)
{
	if (!notify)
		setup_notification();
	if (!notify)
		return -1;

	auto *value = new promise<FileNotifyHandle>;
	auto result = value->get_future();

	looper.run_in_looper([this, value, path]() {
		notify->push_register_notification(path, move(*value));
		delete value;
	});

	try
	{
		auto handle = result.get();
		LOGI("Got notification handle: %d\n", handle);
		handlers[handle] = move(func);
		return handle;
	}
	catch (...)
	{
		return -1;
	}
}

vector<ListEntry> NetworkFilesystem::list(const std::string &path)
{
	auto joined = protocol + "://" + path;
	auto socket = Socket::connect("127.0.0.1", 7070);
	if (!socket)
		return {};

	unique_ptr<FSList> handler(new FSList(joined, move(socket)));
	auto fut = handler->result.get_future();

	looper.run_in_looper([&]() {
		looper.register_handler(EVENT_OUT, move(handler));
	});

	try
	{
		return fut.get();
	}
	catch (...)
	{
		return {};
	}
}

NetworkFile::~NetworkFile()
{
	unmap();
}

NetworkFile::NetworkFile(Looper &looper, const std::string &path, FileMode mode)
	: path(path), mode(mode), looper(looper)
{
	if (mode == FileMode::ReadWrite)
		throw runtime_error("Unsupported file mode.");

	if (mode == FileMode::ReadOnly)
	{
		if (!reopen())
			throw runtime_error("Failed to connect to server.");
	}
}

void NetworkFile::unmap()
{
	if (mode == FileMode::WriteOnly && has_buffer && need_flush)
	{
		need_flush = false;
		auto socket = Socket::connect("127.0.0.1", 7070);
		if (!socket)
			throw runtime_error("Failed to connect to server.");

		auto handler = unique_ptr<FSWriteCommand>(new FSWriteCommand(path, buffer, move(socket)));
		auto reply = handler->result.get_future();
		looper.run_in_looper([&handler, this]() {
			looper.register_handler(EVENT_OUT | EVENT_IN, move(handler));
		});

		try
		{
			NetFSError error = reply.get();
			if (error != NETFS_ERROR_OK)
				LOGE("Failed to write file: %s\n", path.c_str());
		}
		catch (...)
		{
			LOGE("Failed to write file: %s\n", path.c_str());
		}
	}
}

bool NetworkFile::reopen()
{
	if (mode == FileMode::ReadOnly)
	{
		has_buffer = false;
		auto socket = Socket::connect("127.0.0.1", 7070);
		if (!socket)
			return false;

		auto *handler = new FSReader(path, move(socket));
		future = handler->result.get_future();

		// Capture-by-move would be nice here.
		looper.run_in_looper([handler, this]() {
			looper.register_handler(EVENT_OUT, unique_ptr<FSReader>(handler));
		});
	}
	return true;
}

void *NetworkFile::map_write(size_t size)
{
	has_buffer = true;
	need_flush = true;
	buffer.resize(size);
	return buffer.empty() ? nullptr : buffer.data();
}

void *NetworkFile::map()
{
	try
	{
		if (!has_buffer)
		{
			buffer = future.get();
			has_buffer = true;
		}
		return buffer.empty() ? nullptr : buffer.data();
	}
	catch (...)
	{
		return nullptr;
	}
}

size_t NetworkFile::get_size()
{
	try
	{
		if (!has_buffer)
		{
			buffer = future.get();
			has_buffer = true;
		}
		return buffer.size();
	}
	catch (...)
	{
		return 0;
	}
}

unique_ptr<File> NetworkFilesystem::open(const std::string &path, FileMode mode)
{
	try
	{
		auto joined = protocol + "://" + path;
		return unique_ptr<File>(new NetworkFile(looper, move(joined), mode));
	}
	catch (const std::exception &e)
	{
		LOGE("NetworkFilesystem::open(): %s\n", e.what());
		return {};
	}
}

bool NetworkFilesystem::stat(const std::string &path, FileStat &stat)
{
	auto joined = protocol + "://" + path;
	auto socket = Socket::connect("127.0.0.1", 7070);
	if (!socket)
		return false;

	unique_ptr<FSStat> handler(new FSStat(joined, move(socket)));
	auto fut = handler->result.get_future();

	looper.run_in_looper([&]() {
		looper.register_handler(EVENT_OUT, move(handler));
	});

	try
	{
		stat = fut.get();
		return true;
	}
	catch (...)
	{
		return false;
	}
}

NetworkFilesystem::~NetworkFilesystem()
{
	looper.kill();
	if (looper_thread.joinable())
		looper_thread.join();
}
}