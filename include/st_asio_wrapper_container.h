/*
 * st_asio_wrapper_container.h
 *
 *  Created on: 2016-10-12
 *      Author: youngwolf
 *		email: mail2tao@163.com
 *		QQ: 676218192
 *		Community on QQ: 198941541
 *
 * containers.
 */

#ifndef ST_ASIO_WRAPPER_CONTAINER_H_
#define ST_ASIO_WRAPPER_CONTAINER_H_

#include "st_asio_wrapper_base.h"

#ifndef ST_ASIO_INPUT_QUEUE
#define ST_ASIO_INPUT_QUEUE lock_queue
#endif
#ifndef ST_ASIO_INPUT_CONTAINER
#define ST_ASIO_INPUT_CONTAINER list
#endif
#ifndef ST_ASIO_OUTPUT_QUEUE
#define ST_ASIO_OUTPUT_QUEUE lock_queue
#endif
#ifndef ST_ASIO_OUTPUT_CONTAINER
#define ST_ASIO_OUTPUT_CONTAINER list
#endif

namespace st_asio_wrapper
{

//st_asio_wrapper requires that container must take one and only one template argument.
template <class T> using list = boost::container::list<T>;

class dummy_lockable
{
public:
	typedef boost::lock_guard<dummy_lockable> lock_guard;

	//lockable, dummy
	void lock() const {}
	void unlock() const {}
};

class lockable
{
public:
	typedef boost::lock_guard<lockable> lock_guard;

	//lockable
	void lock() {mutex.lock();}
	void unlock() {mutex.unlock();}

private:
	boost::shared_mutex mutex;
};

//Container must at least has the following functions:
// Container() and Container(size_t) constructor
// size
// empty
// clear
// swap
// push_back(const T& item)
// push_back(T&& item)
// splice(Container::const_iterator, std::list<T>&), after this, std::list<T> must be empty
// front
// pop_front
template<typename T, typename Container, typename Lockable>
class queue : public Container, public Lockable
{
public:
	typedef T data_type;
	typedef Container super;
	typedef queue<T, Container, Lockable> me;

	queue() {}
	queue(size_t size) : super(size) {}

	bool enqueue(const T& item) {typename Lockable::lock_guard lock(*this); return enqueue_(item);}
	bool enqueue(T&& item) {typename Lockable::lock_guard lock(*this); return enqueue_(std::move(item));}
	void move_items_in(boost::container::list<T>& can) {typename Lockable::lock_guard lock(*this); move_items_in_(can);}
	bool try_dequeue(T& item) {typename Lockable::lock_guard lock(*this); return try_dequeue_(item);}

	bool enqueue_(const T& item) {this->push_back(item); return true;}
	bool enqueue_(T&& item) {this->push_back(std::move(item)); return true;}
	void move_items_in_(boost::container::list<T>& can) {this->splice(std::end(*this), can);}
	bool try_dequeue_(T& item) {if (this->empty()) return false; item.swap(this->front()); this->pop_front(); return true;}
};

template<typename T, typename Container> using non_lock_queue = queue<T, Container, dummy_lockable>; //totally not thread safe
template<typename T, typename Container> using lock_queue = queue<T, Container, lockable>;

} //namespace

#endif /* ST_ASIO_WRAPPER_CONTAINER_H_ */
