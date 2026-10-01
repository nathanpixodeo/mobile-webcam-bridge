/// Array-backed FIFO with amortised O(1) `popFirst`. The standard library has no deque and
/// BridgeKit deliberately takes no external dependencies.
struct FIFOQueue<Element> {
    private var storage: [Element] = []
    private var head = 0

    var isEmpty: Bool { head >= storage.count }
    var count: Int { storage.count - head }
    var first: Element? { isEmpty ? nil : storage[head] }
    var elements: ArraySlice<Element> { storage[head...] }

    mutating func append(_ element: Element) {
        storage.append(element)
    }

    mutating func popFirst() -> Element? {
        guard !isEmpty else { return nil }
        let element = storage[head]
        head += 1
        compactIfNeeded()
        return element
    }

    /// Removes the matching elements, keeping the order of the rest; returns what was removed.
    @discardableResult
    mutating func removeAll(where shouldRemove: (Element) -> Bool) -> [Element] {
        var kept: [Element] = []
        var removed: [Element] = []
        kept.reserveCapacity(count)
        for element in storage[head...] {
            if shouldRemove(element) { removed.append(element) } else { kept.append(element) }
        }
        storage = kept
        head = 0
        return removed
    }

    mutating func removeAll() {
        storage.removeAll(keepingCapacity: true)
        head = 0
    }

    private mutating func compactIfNeeded() {
        if head >= 64 && head * 2 >= storage.count {
            storage.removeFirst(head)
            head = 0
        }
    }
}

extension FIFOQueue: Sendable where Element: Sendable {}
