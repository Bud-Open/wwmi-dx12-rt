// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace wwmi
{
	// Compact open-addressed flat hash map with SplitMix64 integer mixing and
	// backward-shift deletion (no tombstones). Built for high-frequency
	// D3D12_CPU_DESCRIPTOR_HANDLE (SIZE_T) -> view metadata lookups without
	// per-node heap allocations or byte-at-a-time FNV-1a hashing.
	template <typename Key, typename Value>
	class FlatViewMap
	{
		struct Slot
		{
			Key key {};
			Value value {};
			bool occupied = false;
		};

		std::unique_ptr<Slot[]> _slots;
		size_t _capacity = 0;
		size_t _size = 0;

		static size_t hash_key(Key key) noexcept
		{
			uint64_t h = static_cast<uint64_t>(key);
			h ^= h >> 33;
			h *= 0xff51afd7ed558ccdULL;
			h ^= h >> 33;
			h *= 0xc4ceb9fe1a85ec53ULL;
			h ^= h >> 33;
			return static_cast<size_t>(h);
		}

		void insert_unique_no_grow(Key key, Value &&value) noexcept
		{
			const size_t mask = _capacity - 1;
			size_t idx = hash_key(key) & mask;
			while (_slots[idx].occupied)
				idx = (idx + 1) & mask;
			_slots[idx].key = key;
			_slots[idx].value = std::move(value);
			_slots[idx].occupied = true;
			++_size;
		}

		void rehash(size_t new_capacity)
		{
			auto new_slots = std::make_unique<Slot[]>(new_capacity);
			auto old_slots = std::move(_slots);
			const size_t old_capacity = _capacity;
			_slots = std::move(new_slots);
			_capacity = new_capacity;
			_size = 0;
			for (size_t i = 0; i < old_capacity; ++i)
			{
				if (old_slots[i].occupied)
					insert_unique_no_grow(old_slots[i].key, std::move(old_slots[i].value));
			}
		}

	public:
		FlatViewMap() = default;
		FlatViewMap(FlatViewMap &&other) noexcept
			: _slots(std::move(other._slots)),
			  _capacity(std::exchange(other._capacity, 0)),
			  _size(std::exchange(other._size, 0))
		{
		}
		FlatViewMap &operator=(FlatViewMap &&other) noexcept
		{
			if (this != &other)
			{
				_slots = std::move(other._slots);
				_capacity = std::exchange(other._capacity, 0);
				_size = std::exchange(other._size, 0);
			}
			return *this;
		}
		FlatViewMap(const FlatViewMap &) = delete;
		FlatViewMap &operator=(const FlatViewMap &) = delete;

		size_t size() const noexcept { return _size; }
		size_t capacity() const noexcept { return _capacity; }
		bool empty() const noexcept { return _size == 0; }

		void clear() noexcept
		{
			if (_capacity == 0 || _size == 0)
				return;
			for (size_t i = 0; i < _capacity; ++i)
			{
				if (_slots[i].occupied)
				{
					_slots[i].occupied = false;
					_slots[i].value = Value {};
				}
			}
			_size = 0;
		}

		const Value *find(Key key) const noexcept
		{
			if (_capacity == 0 || _size == 0)
				return nullptr;
			const size_t mask = _capacity - 1;
			size_t idx = hash_key(key) & mask;
			for (;;)
			{
				const Slot &s = _slots[idx];
				if (!s.occupied)
					return nullptr;
				if (s.key == key)
					return &s.value;
				idx = (idx + 1) & mask;
			}
		}

		Value *find(Key key) noexcept
		{
			return const_cast<Value *>(static_cast<const FlatViewMap *>(this)->find(key));
		}

		// Takes Value by value so callers copying from an entry within the same
		// table cannot trigger use-after-rehash when growth happens.
		void insert_or_assign(Key key, Value value)
		{
			if (_capacity == 0)
				rehash(32);
			const size_t mask = _capacity - 1;
			size_t idx = hash_key(key) & mask;
			for (;;)
			{
				Slot &s = _slots[idx];
				if (!s.occupied)
				{
					if ((_size + 1) * 10 > _capacity * 7)
					{
						rehash(_capacity * 2);
						insert_unique_no_grow(key, std::move(value));
						return;
					}
					s.key = key;
					s.value = std::move(value);
					s.occupied = true;
					++_size;
					return;
				}
				if (s.key == key)
				{
					s.value = std::move(value);
					return;
				}
				idx = (idx + 1) & mask;
			}
		}

		bool erase(Key key) noexcept
		{
			if (_capacity == 0 || _size == 0)
				return false;
			const size_t mask = _capacity - 1;
			size_t idx = hash_key(key) & mask;
			for (;;)
			{
				if (!_slots[idx].occupied)
					return false;
				if (_slots[idx].key == key)
					break;
				idx = (idx + 1) & mask;
			}
			// Backward-shift deletion preserves probe invariants with zero tombstones.
			size_t hole = idx;
			size_t cur = (hole + 1) & mask;
			while (_slots[cur].occupied)
			{
				const size_t home = hash_key(_slots[cur].key) & mask;
				if (((hole - home) & mask) < ((cur - home) & mask))
				{
					_slots[hole] = std::move(_slots[cur]);
					hole = cur;
				}
				cur = (cur + 1) & mask;
			}
			_slots[hole].occupied = false;
			_slots[hole].value = Value {};
			--_size;
			return true;
		}
	};
}
