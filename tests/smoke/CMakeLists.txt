add_test(
    NAME ugdr_client_smoke
    COMMAND ugdr_client
)

add_test(
    NAME ugdr_daemon_smoke
    COMMAND ugdr_daemon --help
)

set_tests_properties(
    ugdr_client_smoke
    ugdr_daemon_smoke
    PROPERTIES
        LABELS "ugdr_smoke"
)
