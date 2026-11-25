// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#if RESHADE_ADDON >= 2
#include <d3d12.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <vector>

class D3D12DescriptorHeap;

namespace wwmi::rt_as
{
	enum class Kind : uint8_t { acceleration_structure, structured_srv, structured_uav };

	struct StructuredView
	{
		uint64_t resource, resource_creation, counter, counter_creation;
		uint64_t first_element, counter_offset_bytes;
		uint32_t elements, stride, format, shader_mapping, flags;
	};

	struct Value
	{
		uint64_t write;
		uint64_t source_heap;
		uint64_t source_write;
		uint32_t source_slot;
		Kind kind;
		union
		{
			uint64_t location;
			StructuredView structured;
		};
	};

	struct HeapBounds
	{
		uint64_t start = 0;
		uint32_t count = 0, increment = 0;

		bool contains(uint64_t address) const noexcept
		{
			return start && address >= start && increment &&
				(address - start) % increment == 0 && (address - start) / increment < count;
		}
	};

	// Exact-slot occupancy bitset (1 bit per descriptor slot) + 64-way sharded
	// sparse maps. Each 64-slot occupancy word is mutated strictly under its
	// owning shard's unique lock, while empty spans short-circuit lock-free.
	struct Slots
	{
		static constexpr uint32_t kShardCount = 64;
		static constexpr uint32_t kShardMask = kShardCount - 1;

		struct alignas(64) Shard
		{
			mutable std::shared_mutex mutex;
			std::map<uint32_t, Value> values;
		};

		const uint32_t num_descriptors;
		const uint32_t word_count;
		std::unique_ptr<std::atomic<uint64_t>[]> occupied;
		std::array<Shard, kShardCount> shards;
		std::atomic<uint64_t> next_write { 0 };

		explicit Slots(uint32_t count)
			: num_descriptors(count),
			  word_count((count + 63u) >> 6),
			  occupied(std::make_unique<std::atomic<uint64_t>[]>(word_count ? word_count : 1u))
		{
			const uint32_t n = word_count ? word_count : 1u;
			for (uint32_t i = 0; i < n; ++i)
				occupied[i].store(0, std::memory_order_relaxed);
		}

		static uint64_t word_mask(uint32_t start_bit, uint32_t bit_count) noexcept
		{
			return (bit_count >= 64u ? ~uint64_t(0) : ((uint64_t(1) << bit_count) - 1u)) << start_bit;
		}

		bool has_slot(uint32_t slot) const noexcept
		{
			return slot < num_descriptors &&
				((occupied[slot >> 6].load(std::memory_order_acquire) & (uint64_t(1) << (slot & 63u))) != 0);
		}

		bool has_any_in_range(uint32_t start, uint32_t count) const noexcept
		{
			if (!count || start >= num_descriptors)
				return false;
			if (count > num_descriptors - start)
				count = num_descriptors - start;
			const uint32_t first_word = start >> 6;
			const uint32_t last_word = (start + count - 1u) >> 6;
			if (first_word == last_word)
			{
				const uint64_t mask = word_mask(start & 63u, count);
				return (occupied[first_word].load(std::memory_order_acquire) & mask) != 0;
			}
			const uint32_t first_bits = 64u - (start & 63u);
			if ((occupied[first_word].load(std::memory_order_acquire) & word_mask(start & 63u, first_bits)) != 0)
				return true;
			for (uint32_t w = first_word + 1u; w < last_word; ++w)
				if (occupied[w].load(std::memory_order_acquire) != 0)
					return true;
			const uint32_t last_bits = ((start + count - 1u) & 63u) + 1u;
			return (occupied[last_word].load(std::memory_order_acquire) & word_mask(0, last_bits)) != 0;
		}

		Shard &shard_for_word(uint32_t word) noexcept { return shards[word & kShardMask]; }
		const Shard &shard_for_word(uint32_t word) const noexcept { return shards[word & kShardMask]; }
	};

	// Owned by the existing native heap registration, not the proxy's allocation.
	struct Heap
	{
		D3D12DescriptorHeap *proxy; // Only dereferenced by the owner of ReShade's internal heaps.
		ID3D12DescriptorHeap *native;
		size_t index;
		uint64_t generation;
		D3D12_DESCRIPTOR_HEAP_DESC desc;
		SIZE_T cpu_start;
		uint64_t gpu_start;
		UINT increment;
		std::atomic<Slots *> sparse { nullptr };

		~Heap() { delete sparse.load(std::memory_order_relaxed); }

		Slots *slots() const { return sparse.load(std::memory_order_acquire); }
		Slots &ensure_slots()
		{
			Slots *current = slots();
			if (current == nullptr)
			{
				auto candidate = std::make_unique<Slots>(desc.NumDescriptors);
				if (sparse.compare_exchange_strong(current, candidate.get(), std::memory_order_acq_rel))
					current = candidate.release();
			}
			return *current;
		}
	};

	struct Range
	{
		Heap *heap = nullptr;
		uint32_t slot = 0;

		bool contains(uint32_t count) const
		{
			return heap != nullptr && count != 0 && slot < heap->desc.NumDescriptors &&
				count <= heap->desc.NumDescriptors - slot;
		}
		Range offset(uint32_t delta) const
		{
			return delta <= UINT32_MAX - slot ? Range { heap, slot + delta } : Range {};
		}
		bool tracked() const { return heap != nullptr && heap->slots() != nullptr; }
		bool has_any_occupied(uint32_t count) const noexcept
		{
			if (!contains(count))
				return false;
			const Slots *state = heap->slots();
			return state != nullptr && state->has_any_in_range(slot, count);
		}
		bool supports_structured() const
		{
			// The first two registrations are ReShade's own GPU view/sampler
			// heaps. Their transient/allocator slots are not original inputs.
			return contains(1) && heap->index >= 2;
		}
	};

	inline void clear(Range dest, uint32_t count = 1)
	{
		if (!dest.contains(count))
			return;
		Slots *state = dest.heap->slots();
		if (state == nullptr || !state->has_any_in_range(dest.slot, count))
			return;
		const uint32_t end_slot = dest.slot + count;
		const uint32_t first_word = dest.slot >> 6;
		const uint32_t last_word = (end_slot - 1u) >> 6;
		for (uint32_t w = first_word; w <= last_word; ++w)
		{
			const uint32_t word_base = w << 6;
			const uint32_t lo = (w == first_word) ? dest.slot : word_base;
			const uint32_t hi = (w == last_word) ? end_slot : (word_base + 64u);
			const uint64_t mask = Slots::word_mask(lo & 63u, hi - lo);
			if ((state->occupied[w].load(std::memory_order_acquire) & mask) == 0)
				continue;
			auto &shard = state->shard_for_word(w);
			const std::unique_lock lock(shard.mutex);
			const uint64_t cur = state->occupied[w].load(std::memory_order_relaxed);
			if ((cur & mask) == 0)
				continue;
			shard.values.erase(shard.values.lower_bound(lo), shard.values.lower_bound(hi));
			state->occupied[w].store(cur & ~mask, std::memory_order_release);
		}
	}

	inline void write_value(Range dest, Value value)
	{
		if (!dest.contains(1))
			return;
		Slots &state = dest.heap->ensure_slots();
		const uint32_t w = dest.slot >> 6;
		const uint64_t bit = uint64_t(1) << (dest.slot & 63u);
		auto &shard = state.shard_for_word(w);
		const std::unique_lock lock(shard.mutex);
		value.write = state.next_write.fetch_add(1, std::memory_order_relaxed) + 1u;
		value.source_heap = dest.heap->generation;
		value.source_write = value.write;
		value.source_slot = dest.slot;
		shard.values.insert_or_assign(dest.slot, value);
		state.occupied[w].store(state.occupied[w].load(std::memory_order_relaxed) | bit, std::memory_order_release);
	}

	inline void write_as(Range dest, uint64_t location)
	{
		Value value {};
		value.kind = Kind::acceleration_structure;
		value.location = location;
		write_value(dest, value);
	}

	inline void write_structured(Range dest, Kind kind, const StructuredView &view)
	{
		Value value {};
		value.kind = kind;
		value.structured = view;
		write_value(dest, value);
	}

	inline void write_srv(Range dest, const D3D12_SHADER_RESOURCE_VIEW_DESC *desc)
	{
		if (desc != nullptr && desc->ViewDimension == D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE)
			write_as(dest, desc->RaytracingAccelerationStructure.Location);
		else
			clear(dest);
	}

	struct Copy
	{
		Range dest;
		uint32_t count;
		std::vector<std::pair<uint32_t, Value>> known;
	};

	// Source reads finish before the native copy; destination writes only follow
	// a forwarded native call. One shard at a time is locked, never a heap pair.
	inline Copy snapshot_copy(Range dest, Range source, uint32_t count)
	{
		Copy copy { dest, count, {} };
		if (!dest.contains(count) || !source.contains(count))
			return copy;
		const Slots *state = source.heap->slots();
		if (state == nullptr || !state->has_any_in_range(source.slot, count))
			return copy;
		const bool structured = dest.supports_structured() &&
			!(source.heap->desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
		const uint32_t end_slot = source.slot + count;
		const uint32_t first_word = source.slot >> 6;
		const uint32_t last_word = (end_slot - 1u) >> 6;
		for (uint32_t w = first_word; w <= last_word; ++w)
		{
			const uint32_t word_base = w << 6;
			const uint32_t lo = (w == first_word) ? source.slot : word_base;
			const uint32_t hi = (w == last_word) ? end_slot : (word_base + 64u);
			const uint64_t mask = Slots::word_mask(lo & 63u, hi - lo);
			if ((state->occupied[w].load(std::memory_order_acquire) & mask) == 0)
				continue;
			const auto &shard = state->shard_for_word(w);
			const std::shared_lock lock(shard.mutex);
			for (auto it = shard.values.lower_bound(lo);
				it != shard.values.end() && it->first < hi; ++it)
				if (it->second.kind == Kind::acceleration_structure || structured)
					copy.known.emplace_back(it->first - source.slot, it->second);
		}
		return copy;
	}

	inline void commit_copy(const Copy &copy)
	{
		const auto &[dest, count, known] = copy;
		if (!dest.contains(count))
			return;
		if (known.empty())
		{
			clear(dest, count);
			return;
		}
		Slots &state = dest.heap->ensure_slots();
		const uint32_t end_slot = dest.slot + count;
		const uint32_t first_word = dest.slot >> 6;
		const uint32_t last_word = (end_slot - 1u) >> 6;
		size_t k_idx = 0;
		const size_t k_total = known.size();
		for (uint32_t w = first_word; w <= last_word; ++w)
		{
			const uint32_t word_base = w << 6;
			const uint32_t lo = (w == first_word) ? dest.slot : word_base;
			const uint32_t hi = (w == last_word) ? end_slot : (word_base + 64u);
			const uint64_t mask = Slots::word_mask(lo & 63u, hi - lo);
			const size_t k_begin = k_idx;
			while (k_idx < k_total && dest.slot + known[k_idx].first < hi)
				++k_idx;
			const bool has_known = (k_idx > k_begin);
			if (!has_known && (state.occupied[w].load(std::memory_order_acquire) & mask) == 0)
				continue;

			auto &shard = state.shard_for_word(w);
			const std::unique_lock lock(shard.mutex);
			const uint64_t old_bits = state.occupied[w].load(std::memory_order_relaxed);
			if (!has_known && (old_bits & mask) == 0)
				continue;

			auto &values = shard.values;
			auto next = values.lower_bound(lo);
			const auto end = values.lower_bound(hi);
			uint64_t new_bits = 0;
			try
			{
				for (size_t i = k_begin; i < k_idx; ++i)
				{
					const auto slot = dest.slot + known[i].first;
					while (next != end && next->first < slot)
						next = values.erase(next); // new value is Unknown
					Value value = known[i].second;
					value.write = state.next_write.fetch_add(1, std::memory_order_relaxed) + 1u;
					if (next != end && next->first == slot)
					{
						next->second = value;
						++next;
					}
					else
						values.emplace_hint(next, slot, value);
					new_bits |= uint64_t(1) << (slot & 63u);
				}
			}
			catch (...)
			{
				values.erase(next, end);
				state.occupied[w].store((old_bits & ~mask) | new_bits, std::memory_order_release);
				throw; // the caller still clears all affected original spans
			}
			values.erase(next, end);
			state.occupied[w].store((old_bits & ~mask) | new_bits, std::memory_order_release);
		}
	}
}
#endif
