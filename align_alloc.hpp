#pragma once

#include <cstdint>
#include <cstddef>
#include <bit>
#include <bitset>
#include <new>
#include <cassert>

namespace gteitelbaum {

constexpr std::size_t PAGE_SIZE = 4096;

template<std::size_t NumBuckets = 6, std::size_t MinRegions = 16>
class align_alloc {
public:
    static constexpr std::size_t MAX_SIZE = std::size_t{1} << NumBuckets;
    static constexpr std::size_t MIN_SIZE = sizeof(void*);
    static constexpr std::size_t MIN_SIZE_LOG2 = std::bit_width(MIN_SIZE) - 1;
    static constexpr std::size_t CHUNKS_PER_PAGE = PAGE_SIZE / MAX_SIZE;
    
    // Calculate metadata size
    static constexpr std::size_t META_SIZE = sizeof(void*) * 2 + sizeof(std::bitset<CHUNKS_PER_PAGE>);
    static constexpr std::size_t META_CHUNKS = (META_SIZE + MAX_SIZE - 1) / MAX_SIZE;
    static constexpr std::size_t USABLE_CHUNKS = CHUNKS_PER_PAGE - META_CHUNKS;
    
    static_assert(MAX_SIZE >= MIN_SIZE, "MAX_SIZE must be at least pointer size");
    static_assert(USABLE_CHUNKS > 0, "Must have usable chunks");

    struct Block {
        union {
            Block* next;
            char data[MAX_SIZE];
        };
    };

    struct Page;

    struct PageMeta {
        align_alloc* arena;
        Page* next;
        std::bitset<USABLE_CHUNKS> used_bitmap;
    };

    struct Page {
        Block chunk[USABLE_CHUNKS];
        PageMeta meta;
    };
    
    static_assert(sizeof(Page) <= PAGE_SIZE, "Page exceeds PAGE_SIZE");

private:
    Block* free_lists_[NumBuckets + 1] = {};
    Page* pages_ = nullptr;
    std::size_t num_empty_ = 0;

    static constexpr std::size_t index_for_size(std::size_t size) {
        if (size <= MIN_SIZE) return MIN_SIZE_LOG2;
        return std::bit_width(size - 1);
    }

    static constexpr std::size_t size_for_index(std::size_t i) {
        return std::size_t{1} << i;
    }

public:
    align_alloc() = default;
    ~align_alloc() { destroy(); }

    align_alloc(const align_alloc&) = delete;
    align_alloc& operator=(const align_alloc&) = delete;
    align_alloc(align_alloc&&) = delete;
    align_alloc& operator=(align_alloc&&) = delete;

    void* alloc(std::size_t size) {
        if (size == 0) size = 1;
        if (size > MAX_SIZE) return nullptr;

        std::size_t i = index_for_size(size);

        std::size_t j = i;
        while (j <= NumBuckets && !free_lists_[j])
            j++;

        if (j > NumBuckets) {
            auto* page = new (std::align_val_t{PAGE_SIZE}) Page{};
            assert((reinterpret_cast<std::uintptr_t>(page) % PAGE_SIZE) == 0);
            page->meta.arena = this;
            page->meta.next = pages_;
            // bitmap starts as 0 (all chunks free on free list)
            pages_ = page;

            for (std::size_t c = USABLE_CHUNKS; c > 0; c--) {
                page->chunk[c-1].next = free_lists_[NumBuckets];
                free_lists_[NumBuckets] = &page->chunk[c-1];
            }

            j = NumBuckets;
        }

        while (j > i) {
            Block* block = free_lists_[j];
            free_lists_[j] = block->next;

            // Mark chunk as in-use when taking from NumBuckets level
            if (j == NumBuckets) {
                auto* page = reinterpret_cast<Page*>(
                    reinterpret_cast<std::uintptr_t>(block) & ~(PAGE_SIZE - 1)
                );
                std::size_t chunk_index = block - &page->chunk[0];
                page->meta.used_bitmap.set(chunk_index);
            }

            j--;
            auto* first = block;
            auto* second = reinterpret_cast<Block*>(
                reinterpret_cast<char*>(block) + size_for_index(j)
            );

            Block** cursor = &free_lists_[j];
            while (*cursor && *cursor < first)
                cursor = &(*cursor)->next;
            first->next = second;
            second->next = *cursor;
            *cursor = first;
        }

        Block* ret = free_lists_[i];
        free_lists_[i] = ret->next;
        
        // Mark chunk as in-use when taking directly from NumBuckets level
        if (i == NumBuckets) {
            auto* page = reinterpret_cast<Page*>(
                reinterpret_cast<std::uintptr_t>(ret) & ~(PAGE_SIZE - 1)
            );
            std::size_t chunk_index = ret - &page->chunk[0];
            page->meta.used_bitmap.set(chunk_index);
        }
        
        return ret;
    }

    void free(void* ptr, std::size_t size) {
        if (!ptr || size == 0 || size > MAX_SIZE) return;

        std::size_t i = index_for_size(size);
        auto* block = static_cast<Block*>(ptr);

        while (i < NumBuckets) {
            auto* buddy = reinterpret_cast<Block*>(
                reinterpret_cast<std::uintptr_t>(block) ^ size_for_index(i)
            );

            Block** cursor = &free_lists_[i];
            while (*cursor && *cursor < buddy)
                cursor = &(*cursor)->next;

            if (*cursor == buddy) {
                *cursor = buddy->next;
                if (buddy < block)
                    block = buddy;
                i++;
            } else {
                break;
            }
        }

        Block** cursor = &free_lists_[i];
        while (*cursor && *cursor < block)
            cursor = &(*cursor)->next;
        block->next = *cursor;
        *cursor = block;

        if (i == NumBuckets) {
            auto* page = reinterpret_cast<Page*>(
                reinterpret_cast<std::uintptr_t>(block) & ~(PAGE_SIZE - 1)
            );
            std::size_t chunk_index = block - &page->chunk[0];
            page->meta.used_bitmap.reset(chunk_index);

            if (page->meta.used_bitmap.none()) {
                num_empty_++;

                if (num_empty_ > MinRegions) {
                    cursor = &free_lists_[NumBuckets];
                    while (*cursor) {
                        auto page_addr = reinterpret_cast<std::uintptr_t>(page);
                        auto block_addr = reinterpret_cast<std::uintptr_t>(*cursor);
                        if ((block_addr & ~(PAGE_SIZE - 1)) == page_addr)
                            *cursor = (*cursor)->next;
                        else
                            cursor = &(*cursor)->next;
                    }

                    Page** pcursor = &pages_;
                    while (*pcursor != page)
                        pcursor = &(*pcursor)->meta.next;
                    *pcursor = page->meta.next;

                    num_empty_--;
                    ::operator delete(page, std::align_val_t{PAGE_SIZE});
                }
            }
        }
    }

    void destroy() {
        for (auto& list : free_lists_)
            list = nullptr;

        while (pages_) {
            auto* next = pages_->meta.next;
            ::operator delete(pages_, std::align_val_t{PAGE_SIZE});
            pages_ = next;
        }

        num_empty_ = 0;
    }

    std::size_t num_pages() const {
        std::size_t count = 0;
        for (auto* p = pages_; p; p = p->meta.next)
            count++;
        return count;
    }

    std::size_t num_empty_pages() const {
        return num_empty_;
    }

    std::size_t free_count(std::size_t bucket) const {
        if (bucket > NumBuckets) return 0;
        std::size_t count = 0;
        for (auto* b = free_lists_[bucket]; b; b = b->next)
            count++;
        return count;
    }
};

} // namespace gteitelbaum
