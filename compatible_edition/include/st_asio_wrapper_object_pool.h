/*
 * st_asio_wrapper_object_pool.h
 *
 *  Created on: 2013-8-7
 *      Author: youngwolf
 *		email: mail2tao@163.com
 *		QQ: 676218192
 *		Community on QQ: 198941541
 *
 * this class used at both client and server endpoint, and in both TCP and UDP socket
 * this class can only manage objects that inherit from st_socket
 */

#ifndef ST_ASIO_WRAPPER_OBJECT_POOL_H_
#define ST_ASIO_WRAPPER_OBJECT_POOL_H_

#include <boost/unordered_set.hpp>

#include "st_asio_wrapper_timer.h"
#include "st_asio_wrapper_service_pump.h"

#ifndef ST_ASIO_MAX_OBJECT_NUM
#define ST_ASIO_MAX_OBJECT_NUM	4096
#elif ST_ASIO_MAX_OBJECT_NUM <= 0
	#error object capacity must be bigger than zero.
#endif

//define ST_ASIO_REUSE_OBJECT macro will enable object pool, all objects in invalid_object_can will never be freed, but kept for reusing,
//otherwise, st_object_pool will free objects in invalid_object_can automatically and periodically, ST_ASIO_FREE_OBJECT_INTERVAL means the interval, unit is second,
//see invalid_object_can at the end of st_object_pool class for more details.
#ifndef ST_ASIO_REUSE_OBJECT
	#ifndef ST_ASIO_FREE_OBJECT_INTERVAL
	#define ST_ASIO_FREE_OBJECT_INTERVAL	60 //seconds
	#elif ST_ASIO_FREE_OBJECT_INTERVAL <= 0
		#error free object interval must be bigger than zero.
	#endif
#endif

//define ST_ASIO_CLEAR_OBJECT_INTERVAL macro to let st_object_pool to invoke clear_obsoleted_object() automatically and periodically
//this feature may affect performance with huge number of objects, so re-write st_server_socket_base::on_recv_error and invoke st_object_pool::del_object()
//is recommended for long-term connection system, but for short-term connection system, you are recommended to open this feature.
//you must define this macro as a value, not just define it, the value means the interval, unit is second
//#define ST_ASIO_CLEAR_OBJECT_INTERVAL		60 //seconds
#if defined(ST_ASIO_CLEAR_OBJECT_INTERVAL) && ST_ASIO_CLEAR_OBJECT_INTERVAL <= 0
	#error clear object interval must be bigger than zero.
#endif

namespace st_asio_wrapper
{

template<typename Object>
class st_object_pool : public st_service_pump::i_service, protected st_timer
{
public:
	typedef boost::shared_ptr<Object> object_type;
	typedef const object_type object_ctype;

protected:
	struct st_object_hasher : public std::unary_function<object_type, size_t>
	{
	public:
		size_t operator()(object_ctype& object_ptr) const {return (size_t) object_ptr->id();}
		size_t operator()(boost::uint_fast64_t id) const {return (size_t) id;}
	};

	struct st_object_equal : public std::binary_function<object_type, object_type, bool>
	{
	public:
		bool operator()(object_ctype& left, object_ctype& right) const {return left->id() == right->id();}
		bool operator()(boost::uint_fast64_t id, object_ctype& right) const {return id == right->id();}
	};

	typedef boost::unordered::unordered_set<object_type, st_object_hasher, st_object_equal> container_type;

protected:
	static const tid TIMER_BEGIN = st_timer::TIMER_END;
	static const tid TIMER_FREE_SOCKET = TIMER_BEGIN;
	static const tid TIMER_CLEAR_SOCKET = TIMER_BEGIN + 1;
	static const tid TIMER_END = TIMER_BEGIN + 10;

	st_object_pool(st_service_pump& service_pump_) : i_service(service_pump_), st_timer(service_pump_), cur_id(-1), max_size_(ST_ASIO_MAX_OBJECT_NUM) {}

	void start()
	{
#ifndef ST_ASIO_REUSE_OBJECT
		set_timer(TIMER_FREE_SOCKET, 1000 * ST_ASIO_FREE_OBJECT_INTERVAL, boost::lambda::if_then_else_return(boost::lambda::bind(&st_object_pool::free_object, this, -1), true, true));
#endif
#ifdef ST_ASIO_CLEAR_OBJECT_INTERVAL
		set_timer(TIMER_CLEAR_SOCKET, 1000 * ST_ASIO_CLEAR_OBJECT_INTERVAL, boost::lambda::if_then_else_return(boost::lambda::bind(&st_object_pool::clear_obsoleted_object, this), true, true));
#endif
	}

	void stop() {stop_all_timer();}

	bool add_object(object_ctype& object_ptr)
	{
		assert(object_ptr && !object_ptr->is_equal_to(-1));

		boost::unique_lock<boost::shared_mutex> lock(object_can_mutex);
		return object_can.size() < max_size_ ? object_can.emplace(object_ptr).second : false;
	}

	//only add object_ptr to invalid_object_can when it's in object_can, this can avoid duplicated items in invalid_object_can, because invalid_object_can is a list,
	//there's no way to check the existence of an item in a list efficiently.
	bool del_object(object_ctype& object_ptr)
	{
		assert(object_ptr);

		boost::unique_lock<boost::shared_mutex> lock(object_can_mutex);
		bool exist = object_can.erase(object_ptr) > 0;
		lock.unlock();

		if (exist)
		{
			boost::unique_lock<boost::shared_mutex> lock(invalid_object_can_mutex);
			invalid_object_can.emplace_back(object_ptr);
		}

		return exist;
	}

	virtual void on_create(object_ctype& object_ptr) {}

	void init_object(object_ctype& object_ptr)
	{
		if (object_ptr)
		{
			object_ptr->id(1 + cur_id.fetch_add(1, boost::memory_order_relaxed));
			on_create(object_ptr);
		}
		else
			unified_out::error_out("create object failed!");
	}

#ifdef ST_ASIO_REUSE_OBJECT
	object_type reuse_object()
	{
		boost::unique_lock<boost::shared_mutex> lock(invalid_object_can_mutex);
		for (BOOST_AUTO(iter, invalid_object_can.begin()); iter != invalid_object_can.end(); ++iter)
			if ((*iter).unique() && (*iter)->obsoleted())
			{
				BOOST_AUTO(object_ptr, *iter);
				invalid_object_can.erase(iter);
				lock.unlock();

				object_ptr->reset();
				return object_ptr;
			}

		return object_type();
	}

	template<typename Arg>
	object_type create_object(Arg& arg)
	{
		BOOST_AUTO(object_ptr, reuse_object());
		if (!object_ptr)
			object_ptr = boost::make_shared<Object>(arg);

		init_object(object_ptr);
		return object_ptr;
	}

	template<typename Arg1, typename Arg2>
	object_type create_object(Arg1& arg1, Arg2& arg2)
	{
		BOOST_AUTO(object_ptr, reuse_object());
		if (!object_ptr)
			object_ptr = boost::make_shared<Object>(arg1, arg2);

		init_object(object_ptr);
		return object_ptr;
	}
#else
	template<typename Arg>
	object_type create_object(Arg& arg)
	{
		BOOST_AUTO(object_ptr, boost::make_shared<Object>(arg));
		init_object(object_ptr);
		return object_ptr;
	}

	template<typename Arg1, typename Arg2>
	object_type create_object(Arg1& arg1, Arg2& arg2)
	{
		BOOST_AUTO(object_ptr, boost::make_shared<Object>(arg1, arg2));
		init_object(object_ptr);
		return object_ptr;
	}
#endif

	object_type create_object() {return create_object(boost::ref(sp));}

public:
	//to configure unordered_set(for example, set factor or reserved size), not thread safe, so must be called before service_pump startup.
	container_type& container() {return object_can;}

	size_t max_size() const {return max_size_;}
	void max_size(size_t _max_size) {max_size_ = _max_size;}

	size_t size()
	{
		boost::shared_lock<boost::shared_mutex> lock(object_can_mutex);
		return object_can.size();
	}

	size_t invalid_object_size()
	{
		boost::shared_lock<boost::shared_mutex> lock(invalid_object_can_mutex);
		return invalid_object_can.size();
	}

	object_type find(boost::uint_fast64_t id)
	{
		boost::shared_lock<boost::shared_mutex> lock(object_can_mutex);
		BOOST_AUTO(iter, object_can.find(id, st_object_hasher(), st_object_equal()));
		return iter != object_can.end() ? *iter : object_type();
	}

	//this method has linear complexity, please note.
	object_type at(size_t index)
	{
		boost::shared_lock<boost::shared_mutex> lock(object_can_mutex);
		assert(index < object_can.size());
		return index < object_can.size() ? *(boost::next(object_can.begin(), index)) : object_type();
	}

	//this method has linear complexity, please note.
	object_type invalid_object_at(size_t index)
	{
		boost::shared_lock<boost::shared_mutex> lock(invalid_object_can_mutex);
		assert(index < invalid_object_can.size());
		return index < invalid_object_can.size() ? *boost::next(invalid_object_can.begin(), index) : object_type();
	}

	//this method has linear complexity, please note.
	object_type invalid_object_find(boost::uint_fast64_t id)
	{
		boost::shared_lock<boost::shared_mutex> lock(invalid_object_can_mutex);
		BOOST_AUTO(iter, std::find_if(invalid_object_can.begin(), invalid_object_can.end(), boost::bind(&Object::is_equal_to, _1, id)));
		return iter == invalid_object_can.end() ? object_type() : *iter;
	}

	//this method has linear complexity, please note.
	object_type invalid_object_pop(boost::uint_fast64_t id)
	{
		boost::shared_lock<boost::shared_mutex> lock(invalid_object_can_mutex);
		BOOST_AUTO(iter, std::find_if(invalid_object_can.begin(), invalid_object_can.end(), boost::bind(&Object::is_equal_to, _1, id)));
		if (iter != invalid_object_can.end())
		{
			BOOST_AUTO(object_ptr, *iter);
			invalid_object_can.erase(iter);
			return object_ptr;
		}
		return object_type();
	}

	void list_all_object() {do_something_to_all(boost::bind(&Object::show_info, _1, "", ""));}

	//Kick out obsoleted objects
	//Consider the following assumptions:
	//1.You didn't invoke del_object in on_recv_error or other places.
	//2.For some reason(I haven't met yet), on_recv_error not been invoked
	//st_object_pool will automatically invoke this function if ST_ASIO_CLEAR_OBJECT_INTERVAL been defined
	size_t clear_obsoleted_object()
	{
		BOOST_TYPEOF(invalid_object_can) objects;

		boost::unique_lock<boost::shared_mutex> lock(object_can_mutex);
		for (BOOST_AUTO(iter, object_can.begin()); iter != object_can.end();)
			if ((*iter)->obsoleted())
			{
				objects.emplace_back(*iter);
				iter = object_can.erase(iter);
			}
			else
				++iter;
		lock.unlock();

		size_t size = objects.size();
		if (0 != size)
		{
			unified_out::warning_out(ST_ASIO_SF " object(s) been kicked out!", size);

			boost::unique_lock<boost::shared_mutex> lock(invalid_object_can_mutex);
			invalid_object_can.splice(invalid_object_can.end(), objects);
		}

		return size;
	}

	//free a specific number of objects
	//if you used object pool(define ST_ASIO_REUSE_OBJECT), you can manually call this function to free some objects after the object pool(invalid_object_size())
	// goes big enough for memory saving(because the objects in invalid_object_can are waiting for reusing and will never be freed).
	//if you don't used object pool, st_object_pool will invoke this function automatically and periodically, so you don't need to invoke this function exactly
	//return affected object number.
	size_t free_object(size_t num = -1)
	{
		size_t num_affected = 0;

		boost::unique_lock<boost::shared_mutex> lock(invalid_object_can_mutex);
		for (BOOST_AUTO(iter, invalid_object_can.begin()); num > 0 && iter != invalid_object_can.end();)
			if ((*iter)->obsoleted())
			{
				--num;
				++num_affected;
				iter = invalid_object_can.erase(iter);
			}
			else
				++iter;
		lock.unlock();

		if (num_affected > 0)
			unified_out::warning_out(ST_ASIO_SF " object(s) been freed!", num_affected);

		return num_affected;
	}

	DO_SOMETHING_TO_ALL_MUTEX(object_can, object_can_mutex)
	DO_SOMETHING_TO_ONE_MUTEX(object_can, object_can_mutex)

protected:
	st_atomic_uint_fast64 cur_id;

	container_type object_can;
	boost::shared_mutex object_can_mutex;
	size_t max_size_;

	//because all objects are dynamic created and stored in object_can, maybe when receiving error occur
	//(you are recommended to delete the object from object_can, for example via st_server_base::del_client), some other asynchronous calls are still queued in boost::asio::io_service,
	//and will be dequeued in the future, we must guarantee these objects not be freed from the heap or reused, so we move these objects from object_can to invalid_object_can,
	//and free them from the heap or reuse them in the near future.
	//if ST_ASIO_CLEAR_OBJECT_INTERVAL been defined, clear_obsoleted_object() will be invoked automatically and periodically to move all invalid objects into invalid_object_can.
	boost::container::list<object_type> invalid_object_can;
	boost::shared_mutex invalid_object_can_mutex;
};

} //namespace

#endif /* ST_ASIO_WRAPPER_OBJECT_POOL_H_ */
