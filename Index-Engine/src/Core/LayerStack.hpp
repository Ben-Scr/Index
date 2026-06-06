#pragma once

#include "Core/Export.hpp"
#include "Core/Layer.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace Index {

	// NOT INDEX_API: exporting a class with vector<unique_ptr<...>> triggers MSVC dllexport / deleted-copy-op errors.
	class LayerStack {
	public:
		using Storage = std::vector<std::unique_ptr<Layer>>;
		using iterator = Storage::iterator;
		using const_iterator = Storage::const_iterator;
		using reverse_iterator = Storage::reverse_iterator;
		using const_reverse_iterator = Storage::const_reverse_iterator;

		void PushLayer(std::unique_ptr<Layer> layer) {
			auto insertIt = m_Layers.begin() + static_cast<std::ptrdiff_t>(m_InsertIndex);
			m_Layers.insert(insertIt, std::move(layer));
			++m_InsertIndex;
		}

		void PushOverlay(std::unique_ptr<Layer> overlay) {
			m_Layers.push_back(std::move(overlay));
		}

		// Deferred-pop during dispatch: raw Layer* snapshots would alias freed memory if a sibling unique_ptr was destroyed mid-iteration.
		bool PopLayer(Layer* layer) {
			auto begin = m_Layers.begin();
			auto end = begin + static_cast<std::ptrdiff_t>(m_InsertIndex);
			auto it = std::find_if(begin, end, [layer](const std::unique_ptr<Layer>& entry) {
				return entry.get() == layer;
			});
			if (it == end) {
				return false;
			}

			if (m_DispatchDepth > 0) {
				m_PendingPop.push_back(layer);
				return true;
			}

			m_Layers.erase(it);
			--m_InsertIndex;
			return true;
		}

		bool PopOverlay(Layer* layer) {
			auto begin = m_Layers.begin() + static_cast<std::ptrdiff_t>(m_InsertIndex);
			auto it = std::find_if(begin, m_Layers.end(), [layer](const std::unique_ptr<Layer>& entry) {
				return entry.get() == layer;
			});
			if (it == m_Layers.end()) {
				return false;
			}

			if (m_DispatchDepth > 0) {
				m_PendingPop.push_back(layer);
				return true;
			}

			m_Layers.erase(it);
			return true;
		}

		void Clear() {
			m_Layers.clear();
			m_PendingPop.clear();
			m_InsertIndex = 0;
		}

		bool Empty() const { return m_Layers.empty(); }
		std::size_t Size() const { return m_Layers.size(); }

		void BeginDispatch() {
			++m_DispatchDepth;
		}

		void EndDispatch() {
			if (m_DispatchDepth == 0) {
				return;
			}
			--m_DispatchDepth;
			if (m_DispatchDepth > 0 || m_PendingPop.empty()) {
				return;
			}

			for (Layer* layer : m_PendingPop) {
				auto begin = m_Layers.begin();
				auto insertEnd = begin + static_cast<std::ptrdiff_t>(m_InsertIndex);
				auto it = std::find_if(begin, insertEnd, [layer](const std::unique_ptr<Layer>& entry) {
					return entry.get() == layer;
				});
				if (it != insertEnd) {
					m_Layers.erase(it);
					--m_InsertIndex;
					continue;
				}

				auto overlayBegin = m_Layers.begin() + static_cast<std::ptrdiff_t>(m_InsertIndex);
				auto overlayIt = std::find_if(overlayBegin, m_Layers.end(), [layer](const std::unique_ptr<Layer>& entry) {
					return entry.get() == layer;
				});
				if (overlayIt != m_Layers.end()) {
					m_Layers.erase(overlayIt);
				}
			}
			m_PendingPop.clear();
		}

		bool IsPendingPop(Layer* layer) const {
			if (m_PendingPop.empty()) {
				return false;
			}
			return std::find(m_PendingPop.begin(), m_PendingPop.end(), layer) != m_PendingPop.end();
		}

		Layer* At(std::size_t index) {
			return index < m_Layers.size() ? m_Layers[index].get() : nullptr;
		}
		const Layer* At(std::size_t index) const {
			return index < m_Layers.size() ? m_Layers[index].get() : nullptr;
		}

		iterator begin() { return m_Layers.begin(); }
		iterator end() { return m_Layers.end(); }
		const_iterator begin() const { return m_Layers.begin(); }
		const_iterator end() const { return m_Layers.end(); }
		const_iterator cbegin() const { return m_Layers.cbegin(); }
		const_iterator cend() const { return m_Layers.cend(); }

		reverse_iterator rbegin() { return m_Layers.rbegin(); }
		reverse_iterator rend() { return m_Layers.rend(); }
		const_reverse_iterator rbegin() const { return m_Layers.rbegin(); }
		const_reverse_iterator rend() const { return m_Layers.rend(); }
		const_reverse_iterator crbegin() const { return m_Layers.crbegin(); }
		const_reverse_iterator crend() const { return m_Layers.crend(); }

	private:
		Storage m_Layers;
		std::size_t m_InsertIndex = 0;
		std::size_t m_DispatchDepth = 0;
		std::vector<Layer*> m_PendingPop;
	};

} // namespace Index
