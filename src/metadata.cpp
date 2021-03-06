#include<string>
#include <cstdlib>
#include "raft.hpp"

#define __METADATA_EXT__ ".meta"

#ifndef __MAGIC_START__
#define __MAGIC_START__ 123456789
#endif // __MAGIC_START__ 

#ifndef __MAGIC_END__
#define __MAGIC_END__ 987654321
#endif // __MAGIC_END__

#define APPLIED_INDEX	1
#define COMMITTED_INDEX 2
#define VOTE_FOR		3
#define CURRENT_TERM	4
#define PEER_INFO       5

#define CURRENT_TERM_LEN    \
(sizeof(int) * 2 + sizeof(term_t) + sizeof(char))

#define COMMITTED_INDEX_LEN \
(sizeof(int) * 2 + sizeof(log_index_t) + sizeof(char))

#define APPLIED_INDEX_LEN	\
(sizeof(int) * 2 + sizeof(log_index_t) + sizeof(char))

#define METADATA_SECTION 102
/*
metafile format:

|__MAGIC_START__|char{value type}|value|char{id of value}|__MAGIC_START__|


*/
namespace raft
{

	metadata::metadata(size_t max_file_size)
	{
		current_term_    = 0;
		committed_index_ = 0;
		applied_index_   = 0;
        vote_term_       = 0;

		write_pos_  = 0;
		buf_        = 0;
		file_index_ = 0;

		max_buffer_size_ = max_file_size;
	}
    metadata::~metadata ()
    {
        if(buf_)
        {
            close_mmap(buf_, max_buffer_size_);
        }
    }
	bool metadata::reload(const std::string &path)
	{
        path_ = path;

        //list metadata files.
		std::set<std::string> files
                = list_dir(path, __METADATA_EXT__);

		if (files.empty())
		{
			acl::string file;
			file_index_++;
			file.format("%s%lu%s",
				path.c_str(), 
				file_index_, 
				__METADATA_EXT__);

			if (open(file.c_str(), true))
			{
				if(check_point())
                {
                    file_path_ = file;
                    return true;
                }
			}
			logger_error("create metadata file error");
			return false;
		}
        std::map<size_t, std::string> metafile;

        for (std::set<std::string>::reverse_iterator it =files.rbegin();
             it != files.rend(); ++it)
        {
            logger("--> %s", it->c_str());
            std::string tmp = get_filename(*it);
            metafile.insert(std::make_pair(atoll(tmp.c_str()), *it));
        }

		for (std::map<size_t, std::string>::reverse_iterator
                     it = metafile.rbegin();it != metafile.rend();++it)
		{
			//reload the new metadata file.
			//the new one in the files last one.
			if (!open(it->second, false))
			{
				logger_error("open file error. "
							 "file_path(%s)",
							 it->second.c_str());
				continue;
			}
			if (reload())
			{
                file_index_ = it->first;
                file_path_ = it->second;
                logger("reload_file ok."
                       "file_index(%lu) file_path(%s)",
                       file_index_,
                       it->second.c_str());
				return true;
			}
		}
		return  true;
	}
	bool metadata::reload()
	{
        size_t count = 0;
        acl_assert(write_pos_ );

		do
		{
			//check if reach end of file.
            size_t diff = write_pos_ - buf_;
			if (diff >= max_buffer_size_)
            {
                logger("read end of file.reload ok");
                return true;
            }

			//if has not magic num. it has not data anymore.
			if (get_uint32(write_pos_) != __MAGIC_START__)
            {
                //get_uint32 will move write_pos_ forward
                //read failed. reset write_pos_ to the end
                write_pos_ -= sizeof(__MAGIC_START__);
                logger("reload ok. "
                       "metadata entry count :%lu",
                       count);
                break;
            }
            count ++;

            char ch = get_uint8(write_pos_);
			switch (ch)
			{
			case COMMITTED_INDEX:
			{
				if (load_committed_index())
					continue;
                logger_error("load_committed_index error");
				return false;
			}
			case APPLIED_INDEX:
			{
				if (load_applied_index())
					continue;
                logger_error("load_applied_index error");
				return false;
			}
			case VOTE_FOR:
			{
				if (load_vote_for())
					continue;
                logger_error("load_vote_for error");
				return false;
			}
			case CURRENT_TERM:
			{
				if (load_current_term())
					continue;
                logger_error("load_current_term error");
				return false;
			}case PEER_INFO:
            {
                if(load_peer_info())
                    continue;
                logger_error("load_peer_info error");
                return false;
            }
			default:
				break;
			}
		} while (true);
        return true;
	}


    void metadata::print_status()
    {

        acl::string buffer;
        for(size_t i = 0; i < peer_infos_.size(); i++)
        {
            buffer.format_append("---> [peer_id_(%s)  peer_addr_(%s)]\n",
                                 peer_infos_[i].peer_id_.c_str(),
                                 peer_infos_[i].addr_.c_str());
        }

        logger("\n"
               "---> current_term_(%llu) \n"
               "---> applied_index_(%llu) \n"
               "---> committed_index_(%llu) \n"
               "---> vote_for_(%s) \n"
               "---> vote_term_(%llu) \n"
               "%s",
               current_term_,
               applied_index_,
               committed_index_,
               vote_for_.c_str(),
               vote_term_,
               buffer.c_str());

    }

	bool metadata::load_committed_index()
	{
		committed_index_ = get_uint64(write_pos_);
		if (get_uint32(write_pos_) != __MAGIC_END__)
		{
			logger_error("metadata broken!!!");
			return false;
		}
		return true;
	}

    bool metadata::load_applied_index()
	{
		applied_index_ = get_uint64(write_pos_);
		if (get_uint32(write_pos_) != __MAGIC_END__)
		{
			logger_error("metadata broken!!!");
			return false;
		}
		return true;
	}

	bool metadata::load_current_term()
	{
		current_term_ = get_uint64(write_pos_);
		if (get_uint32(write_pos_) != __MAGIC_END__)
		{
			logger_error("metadata broken!!!");
			return false;
		}
		return true;
	}

    bool metadata::load_peer_info()
    {
        std::vector<peer_info> infos;
        unsigned int size = get_uint32(write_pos_);
        for (unsigned int i = 0; i < size; ++i)
        {
            peer_info info;
            info.peer_id_ = get_string(write_pos_);
            info.addr_    = get_string(write_pos_);
            infos.push_back(info);
        }
        if (get_uint32(write_pos_) != __MAGIC_END__)
        {
            logger_error("metadata broken!!!");
            return false;
        }
        peer_infos_ = infos;
        return true;
    }

	bool metadata::load_vote_for()
	{
		vote_term_ = get_uint64(write_pos_);
		vote_for_ = get_string(write_pos_);
		if (get_uint32(write_pos_) != __MAGIC_END__)
		{
			logger_error("metadata broken!!!");
			return false;
		}
		return true;
	}

    bool metadata::create_new_file ()
    {
        acl::string file_path;

        file_index_ ++;
        file_path.format("%s%lu%s",
                         path_.c_str(),
                         file_index_,
                         __METADATA_EXT__);

        //close the old file mmap
        close_mmap(buf_, max_buffer_size_);
        buf_ = write_pos_ = NULL;

        if(!open(file_path.c_str(), true))
        {
            logger_error("create file error. "
                         "file_path:%s."
                         " error:%s",
                         file_path.c_str(),
                         acl::last_serror());
            return false;
        }
        //write current status to new file;
        if(!check_point())
        {
            logger_fatal("check_point failed");
            return false;
        }

        //delete old file from disk
        if(remove(file_path_.c_str()) != 0)
        {
            logger_error("remove old metadata file(%s) error.%s",
                         file_path_.c_str(),
                         acl::last_serror());
        }
        file_path_ = file_path;

        return true;
    }

	bool metadata::check_remain_buffer(size_t size)
	{
        if(write_pos_ == NULL)
        {
            logger_error("metadata not open");
            return false;
        }

        //end of file
        if((write_pos_ - buf_) == max_buffer_size_ )
            return false;

        size_t diff = write_pos_ - buf_;
         acl_assert(diff < max_buffer_size_);

        size_t remain = max_buffer_size_ -(write_pos_ - buf_);

        /**
         * 4 bytes(sizeof(int)) length buffer for reading
         * next __MAGIC_START__.entry in file begin with
         * __MAGIC_START__
         * when metadata reload file and the data full
         * of the file. read next __MAGIC_START__ will
         * over fo mmap.it will crash
         */
        if(remain > size + sizeof(int))
            return true;

        return create_new_file();
	}

	bool metadata::set_committed_index(log_index_t index)
	{
		acl::lock_guard lg(locker_);
		if (!check_remain_buffer(COMMITTED_INDEX_LEN))
		{
			logger_error("write metadata error");
			return false;
		}
			
		put_uint32(write_pos_, __MAGIC_START__);
		put_uint8(write_pos_, COMMITTED_INDEX);
		put_uint64(write_pos_, index);
		put_uint32(write_pos_, __MAGIC_END__);
		committed_index_ = index;
		return true;
	}

	log_index_t metadata::get_committed_index()
	{
		acl::lock_guard lg(locker_);
		return committed_index_;
	}

	bool metadata::set_applied_index(log_index_t index)
	{
		acl::lock_guard lg(locker_);
		if (!check_remain_buffer(APPLIED_INDEX_LEN))
		{
            logger_error("write metadata error");
			return false;
		}

		put_uint32(write_pos_, __MAGIC_START__);
		put_uint8(write_pos_, APPLIED_INDEX);
		put_uint64(write_pos_, index);
		put_uint32(write_pos_, __MAGIC_END__);
		applied_index_ = index;
		return true;
	}

	log_index_t metadata::get_applied_index()
	{
		acl::lock_guard lg(locker_);
		return applied_index_;
	}

	bool metadata::set_current_term(term_t term)
	{
		acl::lock_guard lg(locker_);
		if (!check_remain_buffer(CURRENT_TERM_LEN))
		{
            logger_error("write metadata error");
			return false;
		}

		put_uint32(write_pos_, __MAGIC_START__);
		put_uint8(write_pos_, CURRENT_TERM);
		put_uint64(write_pos_, term);
		put_uint32(write_pos_, __MAGIC_END__);
		current_term_ = term;
		return true;
	}

	term_t metadata::get_current_term()
	{
		acl::lock_guard lg(locker_);
		return current_term_;
	}

	bool metadata::set_vote_for(const std::string &id, term_t term)
	{
		acl::lock_guard lg(locker_);

		size_t len = APPLIED_INDEX_LEN + sizeof(int) + id.size();
		if (!check_remain_buffer(len))
		{
            logger_error("write metadata error");
			return false;
		}

		put_uint32(write_pos_, __MAGIC_START__);
		put_uint8(write_pos_, VOTE_FOR);
		put_uint64(write_pos_, term);
		put_string(write_pos_, id);
		put_uint32(write_pos_, __MAGIC_END__);
		vote_for_ = id;
		vote_term_ = term;

		return true;
	}

	std::pair<term_t, std::string> metadata::get_vote_for()
	{
		acl::lock_guard lg(locker_);
		return std::make_pair(vote_term_, vote_for_);
	}

    bool metadata::set_peer_infos(const std::vector<peer_info> &infos)
    {
        acl::lock_guard lg(locker_);

        size_t len = sizeof(int);

        for (size_t i = 0; i < infos.size(); ++i)
        {
            len += sizeof(unsigned int);
            len += infos[i].addr_.size();

            len += sizeof(unsigned int);
            len += infos[i].peer_id_.size();
        }

        len += sizeof(unsigned int) * 2 + sizeof(char);

        if (!check_remain_buffer(len))
        {
            logger_error("write metadata error");
            return false;
        }

        put_uint32(write_pos_, __MAGIC_START__);
        put_uint8(write_pos_, PEER_INFO);
        //write vector size
        put_uint32(write_pos_, (unsigned int) infos.size());
        //write peer_info entry
        for (size_t j = 0; j < infos.size(); ++j)
        {
            put_string(write_pos_, infos[j].peer_id_);
            put_string(write_pos_, infos[j].addr_);
        }
        put_uint32(write_pos_, __MAGIC_END__);
        peer_infos_ = infos;
        return true;
    }

    std::vector<peer_info> metadata::get_peer_info()
    {
        acl::lock_guard lg(locker_);
        return peer_infos_;
    }

	bool metadata::open(const std::string &file_path, bool create)
	{
		long long size = acl_file_size(file_path.c_str());
		//file not exist
		if (size == -1 )
		{
			logger_debug(METADATA_SECTION, 10,
                         "open file(%s) "
                         "failed:%s",
                         file_path.c_str(),
                         acl::last_serror());

			if (!create)
				return false;

			//default max size of metadata file
			size = (long long int) max_buffer_size_;
		}

		ACL_FILE_HANDLE fd = acl_file_open( 
			file_path.c_str(), O_RDWR | O_CREAT,0600);

		if (fd == ACL_FILE_INVALID)
		{
            logger("create new file(%s) failed:%s",
                   file_path.c_str(),
                   acl::last_serror());
			return false;
		}

		//open mmap
		write_pos_ = buf_ = 
			static_cast<unsigned char *>(open_mmap(fd, size));
		
		//close fd.we don't need anymore
		acl_file_close(fd);

		return buf_ != NULL;
	}

	bool metadata::check_point()
	{
		return 
			set_applied_index(applied_index_) && 
			set_committed_index(committed_index_) && 
			set_current_term(current_term_) && 
			set_vote_for(vote_for_, vote_term_);
	}
}