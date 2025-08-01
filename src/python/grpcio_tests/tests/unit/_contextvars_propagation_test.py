# Copyright 2020 The gRPC authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Test of propagation of contextvars to AuthMetadataPlugin threads."""

import contextlib
import logging
import os
import queue
import tempfile
import threading
import unittest

import grpc

from tests.unit import test_common

_SERVICE_NAME = "test"
_UNARY_UNARY = "UnaryUnary"
_REQUEST = b"0000"
_UDS_PATH = os.path.join(tempfile.mkdtemp(), "grpc_fullstack_test.sock")


def _unary_unary_handler(request, context):
    return request


def contextvars_supported():
    try:
        import contextvars

        return True
    except ImportError:
        return False


_METHOD_HANDLERS = {
    _UNARY_UNARY: grpc.unary_unary_rpc_method_handler(_unary_unary_handler)
}


@contextlib.contextmanager
def _server():
    try:
        server = test_common.test_server()
        server.add_registered_method_handlers(_SERVICE_NAME, _METHOD_HANDLERS)
        server_creds = grpc.local_server_credentials(
            grpc.LocalConnectionType.UDS
        )
        server.add_secure_port(f"unix:{_UDS_PATH}", server_creds)
        server.start()
        yield _UDS_PATH
    finally:
        server.stop(None)
        if os.path.exists(_UDS_PATH):
            os.remove(_UDS_PATH)


if contextvars_supported():
    import contextvars

    _EXPECTED_VALUE = 24601
    test_var = contextvars.ContextVar("test_var", default=None)

    def set_up_expected_context():
        test_var.set(_EXPECTED_VALUE)

    class TestCallCredentials(grpc.AuthMetadataPlugin):
        def __call__(self, context, callback):
            if (
                test_var.get() != _EXPECTED_VALUE
                and not test_common.running_under_gevent()
            ):
                # contextvars do not work under gevent, but the rest of this
                # test is still valuable as a test of concurrent runs of the
                # metadata credentials code path.
                raise AssertionError(
                    "{} != {}".format(test_var.get(), _EXPECTED_VALUE)
                )
            callback((), None)

        def assert_called(self, test):
            test.assertTrue(self._invoked)
            test.assertEqual(_EXPECTED_VALUE, self._recorded_value)

else:

    def set_up_expected_context():
        pass

    class TestCallCredentials(grpc.AuthMetadataPlugin):
        def __call__(self, context, callback):
            callback((), None)


# TODO(https://github.com/grpc/grpc/issues/22257)
@unittest.skipIf(os.name == "nt", "LocalCredentials not supported on Windows.")
class ContextVarsPropagationTest(unittest.TestCase):
    def test_propagation_to_auth_plugin(self):
        set_up_expected_context()
        with _server() as uds_path:
            local_credentials = grpc.local_channel_credentials(
                grpc.LocalConnectionType.UDS
            )
            test_call_credentials = TestCallCredentials()
            call_credentials = grpc.metadata_call_credentials(
                test_call_credentials, "test call credentials"
            )
            composite_credentials = grpc.composite_channel_credentials(
                local_credentials, call_credentials
            )
            with grpc.secure_channel(
                f"unix:{uds_path}", composite_credentials
            ) as channel:
                stub = channel.unary_unary(
                    grpc._common.fully_qualified_method(
                        _SERVICE_NAME, _UNARY_UNARY
                    ),
                    _registered_method=True,
                )
                response = stub(_REQUEST, wait_for_ready=True)
                self.assertEqual(_REQUEST, response)

    def test_concurrent_propagation(self):
        _THREAD_COUNT = 32
        _RPC_COUNT = 32

        set_up_expected_context()
        with _server() as uds_path:
            local_credentials = grpc.local_channel_credentials(
                grpc.LocalConnectionType.UDS
            )
            test_call_credentials = TestCallCredentials()
            call_credentials = grpc.metadata_call_credentials(
                test_call_credentials, "test call credentials"
            )
            composite_credentials = grpc.composite_channel_credentials(
                local_credentials, call_credentials
            )
            wait_group = test_common.WaitGroup(_THREAD_COUNT)

            def _run_on_thread(exception_queue):
                try:
                    with grpc.secure_channel(
                        f"unix:{uds_path}", composite_credentials
                    ) as channel:
                        stub = channel.unary_unary(
                            grpc._common.fully_qualified_method(
                                _SERVICE_NAME, _UNARY_UNARY
                            ),
                            _registered_method=True,
                        )
                        wait_group.done()
                        wait_group.wait()
                        for i in range(_RPC_COUNT):
                            response = stub(_REQUEST, wait_for_ready=True)
                            self.assertEqual(_REQUEST, response)
                except Exception as e:  # pylint: disable=broad-except
                    exception_queue.put(e)

            threads = []

            for _ in range(_THREAD_COUNT):
                q = queue.Queue()
                thread = threading.Thread(target=_run_on_thread, args=(q,))
                thread.daemon = True
                thread.start()
                threads.append((thread, q))

            for thread, q in threads:
                thread.join()
                if not q.empty():
                    raise q.get()


if __name__ == "__main__":
    logging.basicConfig()
    unittest.main(verbosity=2)
