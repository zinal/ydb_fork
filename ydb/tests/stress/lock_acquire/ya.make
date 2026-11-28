PY3_PROGRAM(lock_acquire_test)

STYLE_PYTHON()

PY_SRCS(
    __main__.py
)

PEERDIR(
    ydb/public/sdk/python
    ydb/public/sdk/python/enable_v3_new_behavior
)

END()
