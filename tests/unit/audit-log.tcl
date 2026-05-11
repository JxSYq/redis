start_server {tags {"audit-log"}} {
    test {CONFIG GET audit-log-enabled default} {
        assert_equal "no" [lindex [r config get audit-log-enabled] 1]
    }

    test {CONFIG SET audit-log-enabled yes} {
        r config set audit-log-enabled yes
        assert_equal "yes" [lindex [r config get audit-log-enabled] 1]
    }

    test {CONFIG SET audit-log-enabled no} {
        r config set audit-log-enabled no
        assert_equal "no" [lindex [r config get audit-log-enabled] 1]
    }

    test {CONFIG GET audit-log-path default} {
        assert_equal "" [lindex [r config get audit-log-path] 1]
    }

    test {CONFIG SET audit-log-path} {
        r config set audit-log-path /tmp/test_audit.log
        assert_equal "/tmp/test_audit.log" [lindex [r config get audit-log-path] 1]
    }

    test {CONFIG GET audit-log-encrypt-enabled default} {
        assert_equal "yes" [lindex [r config get audit-log-encrypt-enabled] 1]
    }

    test {CONFIG SET audit-log-encrypt-enabled no} {
        r config set audit-log-encrypt-enabled no
        assert_equal "no" [lindex [r config get audit-log-encrypt-enabled] 1]
    }

    test {CONFIG SET audit-log-encrypt-enabled back to yes} {
        r config set audit-log-encrypt-enabled yes
        assert_equal "yes" [lindex [r config get audit-log-encrypt-enabled] 1]
    }

    test {CONFIG GET audit-log-queue-length default} {
        assert_equal "100000" [lindex [r config get audit-log-queue-length] 1]
    }

    test {CONFIG SET audit-log-queue-length} {
        r config set audit-log-queue-length 50000
        assert_equal "50000" [lindex [r config get audit-log-queue-length] 1]
    }

    test {CONFIG GET audit-log-customer-command-list default} {
        assert_equal "" [lindex [r config get audit-log-customer-command-list] 1]
    }

    test {CONFIG SET audit-log-customer-command-list} {
        r config set audit-log-customer-command-list "GET INFO"
        assert_equal "GET INFO" [lindex [r config get audit-log-customer-command-list] 1]
    }

    test {CONFIG GET audit-log-abort-count default} {
        assert_equal "0" [lindex [r config get audit-log-abort-count] 1]
    }

    test {CONFIG SET audit-log-abort-count is rejected} {
        catch {r config set audit-log-abort-count 100} err
        set _ $err
    } {ERR*Unsupported CONFIG*}

    # --- File operations tests (Commit 3) ---

    test {Audit log file is created with enabled and valid path} {
        set testpath "/tmp/redis_audit_test_file.log"
        file delete -- $testpath
        r config set audit-log-path $testpath
        r config set audit-log-enabled yes
        after 200
        assert {[file exists $testpath]}
    }

    test {Audit log file has 0600 permissions} {
        set testpath "/tmp/redis_audit_test_perm.log"
        file delete -- $testpath
        r config set audit-log-path $testpath
        r config set audit-log-enabled yes
        after 200
        set perms [file attributes $testpath -permissions]
        assert {[string match *0600 $perms] || [string match *00600 $perms]}
    }

    test {Audit log file auto-creates parent directories} {
        set subdir "/tmp/redis_audit_nested/sub/dir"
        set testpath "$subdir/test.log"
        file delete -force -- /tmp/redis_audit_nested
        r config set audit-log-path $testpath
        r config set audit-log-enabled yes
        after 200
        assert {[file isdirectory $subdir]}
        assert {[file exists $testpath]}
    }

    test {Audit log path switch: old closed, new opened} {
        set path1 "/tmp/redis_audit_switch1.log"
        set path2 "/tmp/redis_audit_switch2.log"
        file delete -- $path1 $path2
        r config set audit-log-enabled yes
        r config set audit-log-path $path1
        after 200
        assert {[file exists $path1]}
        r config set audit-log-path $path2
        after 200
        assert {[file exists $path2]}
    }

    test {Audit log disable closes file} {
        set testpath "/tmp/redis_audit_disable.log"
        file delete -- $testpath
        r config set audit-log-path $testpath
        r config set audit-log-enabled yes
        after 200
        assert {[file exists $testpath]}
        r config set audit-log-enabled no
        after 100
        # File should still exist (not deleted), just closed
        assert {[file exists $testpath]}
    }

    test {Audit log re-enable re-opens file} {
        set testpath "/tmp/redis_audit_reenable.log"
        file delete -- $testpath
        r config set audit-log-path $testpath
        # Enable, then disable, then re-enable
        r config set audit-log-enabled yes
        after 100
        r config set audit-log-enabled no
        after 100
        r config set audit-log-enabled yes
        after 200
        assert {[file exists $testpath]}
    }

    test {Audit log with invalid directory path does not crash server} {
        r config set audit-log-path "/root/no_perm_dir/test.log"
        r config set audit-log-enabled yes
        after 200
        # Server should still be alive - verify with a PING
        assert_equal "PONG" [r ping]
    }

    test {Audit log empty path does not crash} {
        r config set audit-log-path ""
        r config set audit-log-enabled yes
        after 100
        assert_equal "PONG" [r ping]
    }

    test {Audit log queue length rebuild callback works} {
        r config set audit-log-queue-length 1000
        assert_equal "1000" [lindex [r config get audit-log-queue-length] 1]
        r config set audit-log-queue-length 50000
        assert_equal "50000" [lindex [r config get audit-log-queue-length] 1]
        assert_equal "PONG" [r ping]
    }

    # Cleanup
    test {Audit log cleanup test files} {
        file delete -force -- /tmp/redis_audit_nested
        foreach f [glob -nocomplain /tmp/redis_audit_*.log] {
            file delete -- $f
        }
        set _ 1
    } {1}
}
