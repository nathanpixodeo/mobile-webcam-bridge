import BridgeKit
import Foundation
import Network

/// Receives the events of one host connection. Every call happens on the transport queue.
protocol ConnectionDriverDelegate: AnyObject, Sendable {
    func connectionDidOpen(_ id: ConnectionID)
    func connection(_ id: ConnectionID, didReceive data: Data)
    func connection(_ id: ConnectionID, didFinishSending byteCount: Int)
    func connectionDidClose(_ id: ConnectionID, reason: String)
}

/// Owns one `NWConnection`. Started on the transport queue, so Network delivers every callback
/// there and the class needs no locks.
final class ConnectionDriver: @unchecked Sendable {
    let id: ConnectionID
    private let connection: NWConnection
    private let queue: DispatchQueue
    private weak var delegate: (any ConnectionDriverDelegate)?
    private var isClosed = false

    init(id: ConnectionID, connection: NWConnection, queue: DispatchQueue, delegate: any ConnectionDriverDelegate) {
        self.id = id
        self.connection = connection
        self.queue = queue
        self.delegate = delegate
    }

    var remoteDescription: String { "\(connection.endpoint)" }

    func start() {
        connection.stateUpdateHandler = { [weak self] state in self?.handle(state) }
        connection.start(queue: queue)
    }

    /// Hands bytes to Network. `didFinishSending` fires once they are processed, which is what
    /// the send window is measured against.
    func send(_ data: Data) {
        dispatchPrecondition(condition: .onQueue(queue))
        guard !isClosed else { return }
        let byteCount = data.count
        connection.send(content: data, completion: .contentProcessed { [weak self] error in
            guard let self, !self.isClosed else { return }
            if let error {
                self.close(reason: "send failed: \(error)")
            } else {
                self.delegate?.connection(self.id, didFinishSending: byteCount)
            }
        })
    }

    /// Graceful close: data already handed to Network is still delivered before the FIN.
    func close(reason: String) {
        dispatchPrecondition(condition: .onQueue(queue))
        guard !isClosed else { return }
        isClosed = true
        connection.stateUpdateHandler = nil
        connection.cancel()
        delegate?.connectionDidClose(id, reason: reason)
    }

    private func handle(_ state: NWConnection.State) {
        switch state {
        case .ready:
            delegate?.connectionDidOpen(id)
            receiveNext()
        case .failed(let error):
            close(reason: "failed: \(error)")
        case .waiting(let error):
            // An accepted inbound connection that is waiting will not recover.
            close(reason: "waiting: \(error)")
        case .cancelled:
            close(reason: "cancelled")
        default:
            break
        }
    }

    private func receiveNext() {
        connection.receive(minimumIncompleteLength: 1, maximumLength: 64 * 1_024) { [weak self] content, _, isComplete, error in
            guard let self, !self.isClosed else { return }
            if let content, !content.isEmpty {
                self.delegate?.connection(self.id, didReceive: content)
            }
            guard !self.isClosed else { return }
            if let error {
                self.close(reason: "receive failed: \(error)")
            } else if isComplete {
                self.close(reason: "host closed the connection")
            } else {
                self.receiveNext()
            }
        }
    }
}
