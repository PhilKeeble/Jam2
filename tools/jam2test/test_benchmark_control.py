import hashlib
import socket
import tempfile
import threading
import unittest
from pathlib import Path

from jam2test.benchmark_control import JsonLinePeer


class BenchmarkControlTests(unittest.TestCase):
    def test_artifact_transfer_streams_and_preserves_following_frame(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.bin"
            target = root / "target.bin"
            source.write_bytes(bytes(range(251)) * 1000)
            left_socket, right_socket = socket.socketpair()
            sender = JsonLinePeer(left_socket)
            receiver = JsonLinePeer(right_socket)

            def send():
                sender.send_file({"type": "artifact.upload", "size": source.stat().st_size}, source)
                sender.send({"type": "after"})

            thread = threading.Thread(target=send)
            thread.start()
            header = receiver.read()
            digest = receiver.read_to_file(header["size"], target)
            following = receiver.read()
            thread.join(timeout=5)
            sender.close(); receiver.close()
            self.assertEqual(source.read_bytes(), target.read_bytes())
            self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(), digest)
            self.assertEqual("after", following["type"])


if __name__ == "__main__":
    unittest.main()
