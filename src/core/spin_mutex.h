#pragma once
#include "core/lux.h"

namespace Lux
{
	namespace MT
	{
		class LUX_CORE_API SpinMutex LUX_ABSTRACT
		{
		public:
			static SpinMutex* create(bool locked);
			static void destroy(SpinMutex* spin_mutex);

			//hack
			//todo: remove
			static size_t getRequiredSize();
			static SpinMutex* createOnMemory(bool locked, void* ptr);
			static void destruct(SpinMutex* sm);
			///////////////////////////////////////

			virtual void lock() = 0;
			virtual bool poll() = 0;

			virtual void unlock() = 0;

		protected:
			virtual ~SpinMutex() {}
		};

		class SpinLock
		{
		public:
			SpinLock(SpinMutex& mutex) : m_mutex(mutex) { mutex.lock(); }
			~SpinLock() { m_mutex.unlock(); }

		private:
			SpinMutex& m_mutex;
		};
	}; // ~namespace MT
}; // ~namespace Lux