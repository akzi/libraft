#include "raft.hpp"

#define __MAGIC_START__ 123456789
#define __MAGIC_END__   987654321
#define __64k__			64*1024

namespace raft
{

	mmap_log::mmap_log(int max_size /*= __4MB__*/)
	{
		max_data_size_ = 0;
		max_index_size_ = 0;

		if (max_size < __4MB__)
			max_size = __4MB__;

		while (max_data_size_ < max_size)
			max_data_size_ += __64k__;

		max_index_size_ = max_index_size(max_data_size_);

		start_index_ = 0;
		last_index_ = 0;
		eof_ = false;
		is_open_ = false;


		if (!acl_pthread_rwlock_init(&rwlocker_, NULL))
		{
			logger_fatal("acl_pthread_rwlock_init error.%s",
				acl_last_serror());
		}
	}

	mmap_log::~mmap_log()
	{
		if (is_open_)
			close();
	}

	bool mmap_log::open(const std::string &filename)
	{
		ACL_FILE_HANDLE fd = acl_file_open(
			filename.c_str(),
			O_RDWR | O_CREAT,
			0600);

		if (fd == ACL_FILE_INVALID)
		{
			logger_error("open %s error %s\r\n",
				filename.c_str(),
				acl_last_serror());
			return false;
		}

		data_buf_ = data_wbuf_ =
			(unsigned char*)open_mmap(fd, max_data_size_);

		if (!data_buf_)
		{
			logger_error("open_mmap %s error %s\r\n", filename.c_str(),
				acl_last_serror());
			acl_file_close(fd);
			return false;
		}
		acl_file_close(fd);

		std::string indexfile = filename + ".index";
		fd = acl_file_open(indexfile.c_str(), O_RDWR | O_CREAT, 0600);
		if (fd == ACL_FILE_INVALID)
		{
			logger_error("open %s error %s\r\n",
				indexfile.c_str(), acl_last_serror());
			close_mmap(data_buf_);
			data_buf_ = data_wbuf_ = NULL;
			return false;
		}
		index_buf_ = index_wbuf_ = (unsigned char*)open_mmap(
			fd,
			max_index_size_);

		if (!index_buf_)
		{
			logger_error("acl_vstring_mmap_alloc %s error %s\r\n",
				indexfile.c_str(), acl_last_serror());
			close_mmap(data_buf_);
			data_buf_ = data_wbuf_ = NULL;
			acl_file_close(fd);
			return false;
		}
		acl_file_close(fd);
		if (!reload_log())
		{
			logger_error("reload log failed");
			return false;
		}
		is_open_ = true;
		return true;
	}

	bool mmap_log::close()
	{
		acl::lock_guard lg(write_locker_);
		acl_assert(is_open_);
		acl_assert(acl_pthread_rwlock_wrlock(&rwlocker_));


		if (data_buf_)
			close_mmap(data_buf_);
		if (index_buf_)
			close_mmap(index_buf_);

		data_buf_ = data_wbuf_ = NULL;
		index_buf_ = index_wbuf_ = NULL;
		is_open_ = false;
		return true;
	}

	bool mmap_log::write(const log_entry & entry)
	{
		acl::lock_guard lg(write_locker_);

		size_t offset = (data_wbuf_ - data_buf_);
		size_t remail_len = max_data_size_ - offset;
		size_t entry_len = get_sizeof(entry);

		//for __MAGIC_START__, __MAGIC_END__ space
		if (remail_len < entry_len + sizeof(unsigned int) * 2)
		{
			eof_ = true;
			return false;
		}

		if (!is_open_)
		{
			logger("mmap log not open");
			return false;
		}

		if (entry.index() < last_index_)
		{
			logger_error("log_entry error");
			return false;
		}

		put_uint32(data_wbuf_, __MAGIC_START__);
		put_message(data_wbuf_, entry);
		put_uint32(data_wbuf_, __MAGIC_END__);

		put_uint32(index_wbuf_, __MAGIC_START__);
		put_uint64(index_wbuf_, entry.index());
		put_uint32(index_wbuf_, (unsigned int)offset);
		put_uint32(index_wbuf_, __MAGIC_END__);

		last_index_ = entry.index();
		return true;
	}

	bool mmap_log::truncate(log_index_t index)
	{
		acl::lock_guard lg(write_locker_);

		if (!is_open_)
		{
			logger("mmap log not open");
			return false;
		}

		if (index < start_index_ || index > last_index_)
		{
			logger_error("index error");
			return false;
		}

		unsigned char *index_buffer = get_index_buffer(index);
		unsigned char *buffer = index_buffer;

		if (!index_buffer || get_uint32(index_buffer))
			return false;
		//get index
		if (get_uint64(index_buffer) != index)
		{
			logger_fatal("mmap_log error");
			return false;
		}
		//get offset
		get_uint32(index_buffer);
		if (get_uint32(index_buffer) == __MAGIC_END__)
		{
			logger_fatal("mmap_log error");
			return false;
		}

		index_wbuf_ = buffer;

		//write 0 to truncate
		put_uint32(buffer, 0);

		last_index_ = index - 1;

		//update index
		if (start_index_ < last_index_)
			start_index_ = last_index_;

		return move_data_wbuf(last_index_);
	}

	bool mmap_log::read(log_index_t index, 
		int max_bytes, 
		int max_count, 
		std::vector<log_entry> &entries, 
		int &bytes)
	{

		if (max_bytes <= 0 || max_count <= 0)
		{
			logger_error("param error");
			return false;
		}


		if (index < start_index() || index > last_index())
		{
			logger_error("index error,%d", index);
			return false;
		}

		acl_assert(!acl_pthread_rwlock_rdlock(&rwlocker_));

		unsigned char *buffer = get_data_buffer(index);
		if (!buffer)
		{
			acl_assert(!acl_pthread_rwlock_unlock(&rwlocker_));
			return false;
		}

		while (true)
		{
			log_entry entry;

			if (buffer - data_buf_ >=
				(int)(max_data_size_ - one_index_size()))
				break;

			if (!get_entry(buffer, entry))
				break;

			max_bytes -= (int)entry.ByteSizeLong();
			--max_count;

			if (max_bytes <= 0 || max_count <= 0)
				break;

			entries.push_back(entry);
			bytes += (int)entry.ByteSizeLong();

			//read the last one
			if (entry.index() == last_index())
				break;;
		}
		acl_assert(!acl_pthread_rwlock_unlock(&rwlocker_));

		return !!entries.size();
	}

	bool mmap_log::read(log_index_t index, log_entry &entry)
	{
		acl_assert(!acl_pthread_rwlock_rdlock(&rwlocker_));

		if (!is_open_)
		{
			logger("mmap log not open");
			acl_assert(!acl_pthread_rwlock_unlock(&rwlocker_));
			return false;
		}

		unsigned char *data_buf = get_data_buffer(index);

		if (!get_entry(data_buf, entry))
		{
			acl_assert(!acl_pthread_rwlock_unlock(&rwlocker_));
			return false;
		}

		acl_assert(!acl_pthread_rwlock_unlock(&rwlocker_));
		return true;
	}

	bool mmap_log::eof()
	{
		return eof_;
	}

	raft::log_index_t mmap_log::last_index()
	{
		return last_index_;
	}

	raft::log_index_t mmap_log::start_index()
	{
		acl_assert(!acl_pthread_rwlock_rdlock(&rwlocker_));
		if (start_index_ == 0)
			reload_start_index();
		acl_assert(!acl_pthread_rwlock_unlock(&rwlocker_));
		return start_index_;
	}

	bool mmap_log::get_entry(unsigned char *& buffer, log_entry &entry)
	{
		if (get_uint32(buffer) != __MAGIC_START__)
			false;

		if (!get_message(buffer, entry))
		{
			logger_fatal("mmap error");
			false;
		}

		if (get_uint32(buffer) != __MAGIC_END__)
		{
			logger_fatal("mmap error");
			false;
		}
		return true;
	}

	unsigned char* mmap_log::get_data_buffer(log_index_t index)
	{
		unsigned int offset = 0;
		unsigned char *index_buf = get_index_buffer(index);
		if (!index_buf)
			return NULL;

		unsigned int magic = get_uint32(index_buf);
		if (magic != __MAGIC_START__)
			return NULL;

		log_index_t index_value = get_uint64(index_buf);
		if (index_value != index)
		{
			logger_fatal("mmap_log error");
			return NULL;
		}

		offset = get_uint32(index_buf);
		if (get_uint32(index_buf) != __MAGIC_END__)
		{
			logger_fatal("mmap_log error");
			return NULL;
		}

		acl_assert(offset < max_data_size_);
		return data_buf_ + offset;
	}

	size_t mmap_log::max_index_size(size_t max_mmap_size)
	{
		log_entry entry;
		entry.set_index(-1);
		entry.set_term(-1);
		entry.set_type(log_entry_type::e_raft_log);
		entry.set_log_data(std::string(" "));

		size_t one_entry_len = get_sizeof(entry) + sizeof(int) * 2;

		size_t one_index_len = sizeof(long long) + sizeof(int) * 3;

		size_t size = ((max_mmap_size / one_entry_len + 1)* one_index_len);

		size_t max_size = __64k__;

		while (max_size < size)
		{
			max_size += __64k__;
		}

		return max_size;
	}

	size_t mmap_log::one_index_size()
	{
		return sizeof(log_index_t) + sizeof(int) * 3;
	}

	bool mmap_log::reload_log()
	{
		if (get_uint32(index_wbuf_) == __MAGIC_START__)
		{
			//get index
			last_index_ = start_index_ = get_uint64(index_wbuf_);
			//get offset
			get_uint32(index_wbuf_);
			if (get_uint32(index_wbuf_) != __MAGIC_END__)
			{
				logger_error("mmap_error");
				return false;
			}

			size_t max_size = max_index_size_ - sizeof(int);

			while (index_wbuf_ - index_buf_ < (int)max_size)
			{
				if (get_uint32(index_wbuf_) == __MAGIC_START__)
				{
					//index
					last_index_ = get_uint64(index_wbuf_);
					//offset
					get_uint32(index_wbuf_);
					if (get_uint32(index_wbuf_) != __MAGIC_END__)
					{
						logger_fatal("mmap index error");
						return false;
					}
					continue;
				}
				//for get_uint32
				index_wbuf_ -= sizeof(unsigned int);
				break;
			}
		}
		else
		{
			//for get_uint32
			index_wbuf_ -= sizeof(unsigned int);
			return true;
		}
		return move_data_wbuf(last_index_);
	}

	bool mmap_log::move_data_wbuf(log_index_t index)
	{
		//index ==0 for empty
		if (index == 0)
		{
			data_wbuf_ = data_buf_;
			return true;
		}

		log_entry entry;
		unsigned char *data_buffer = get_data_buffer(index);

		if (!data_buffer || get_uint32(data_buffer) != __MAGIC_START__)
		{
			logger_fatal("mmap error");
			return false;
		}

		bool rc = get_message(data_buffer, entry);

		if (get_uint32(data_buffer) != __MAGIC_END__)
		{
			logger_fatal("mmap error");
			return false;
		}
		//set data_wbuf to end
		data_wbuf_ = data_buffer;
		return true;
	}

	void mmap_log::reload_start_index()
	{
		unsigned char *buffer_ptr = index_buf_;
		if (get_uint32(buffer_ptr) == __MAGIC_START__)
		{
			start_index_ = get_uint64(buffer_ptr);
			//get offset
			get_uint32(buffer_ptr);
			if (get_uint32(buffer_ptr) != __MAGIC_END__)
			{
				logger_fatal("mmap_error");
			}
		}
	}

	unsigned char* mmap_log::get_index_buffer(log_index_t index)
	{
		if (start_index_ == 0)
			reload_start_index();

		if (index < start_index_)
			return NULL;
		else if (!start_index_ || index == start_index_)
			return index_buf_;

		size_t offset = (index - start_index_) * one_index_size();

		if (offset >= max_index_size_ - one_index_size())
			return NULL;

		return index_buf_ + offset;
	}

	void * mmap_log::open_mmap(ACL_FILE_HANDLE fd, size_t maxlen)
	{
		void *data = NULL;
		ACL_FILE_HANDLE hmap;

#ifdef ACL_UNIX

		data = mmap(
			NULL,
			maxlen,
			PROT_READ | PROT_WRITE,
			MAP_SHARED,
			fd,
			0);

		if (data == MAP_FAILED)
			logger_error("mmap error: %s", acl_last_serror());

#elif defined(_WIN32) || defined(_WIN64)

		hmap = CreateFileMapping(
			fd,
			NULL,
			PAGE_READWRITE,
			0,
			(DWORD)maxlen,
			NULL);

		if (hmap == NULL)
			logger_error("CreateFileMapping: %s", acl_last_serror());

		data = MapViewOfFile(
			hmap,
			FILE_MAP_READ | FILE_MAP_WRITE,
			0,
			0,
			0);

		if (data == NULL)
			logger_error("MapViewOfFile error: %s",
				acl_last_serror());
#else
		logger_error("%s: not supported yet!", __FUNCTION__);
#endif
		return data;
	}

	void mmap_log::close_mmap(void *map)
	{
#if defined (_WIN32) || defined(_WIN64)
		FlushViewOfFile(map, 0);
		UnmapViewOfFile(map);
#elif defined (ACL_UNIX)
		unmap(map, max_index_size_);
#endif
	}

}