start_server {tags {"commandstats"}} {
    test {INFO commandstats - default state (no extended fields)} {
        r set k1 v1
        r get k1
        set info [r info commandstats]

        # Should have standard fields but no extended ones
        assert_match {*calls=*usec=*usec_per_call=*} $info
        assert_match {*rejected_calls=*failed_calls*} $info

        # Should NOT have extended fields
        assert_no_match {*usec_min=*} $info
        assert_no_match {*usec_p95=*} $info
        assert_no_match {*calls_5s=*} $info

        # Should NOT have aggregation categories
        assert_no_match {*cmdstat_-:*} $info
        assert_no_match {*cmdstat_r:*} $info
        assert_no_match {*cmdstat_w:*} $info
        assert_no_match {*cmdstat_o:*} $info
    }

    test {CONFIG SET command-latency-tracking yes} {
        r config set command-latency-tracking yes
        assert_equal {yes} [lindex [r config get command-latency-tracking] 1]
    }

    test {INFO commandstats - extended fields appear when tracking is enabled} {
        r set k2 v2
        r get k2
        r get k1
        set info [r info commandstats]

        # Extended fields should appear
        assert_match {*usec_min=*} $info
        assert_match {*usec_max=*} $info
        assert_match {*usec_p95=*} $info
        assert_match {*usec_p99=*} $info

        # 5s window fields
        assert_match {*calls_5s=*} $info
        assert_match {*usec_5s=*} $info
        assert_match {*usec_min_5s=*} $info
        assert_match {*usec_avg_5s=*} $info
        assert_match {*usec_max_5s=*} $info

        # 1min window fields
        assert_match {*calls_minute=*} $info
        assert_match {*usec_minute=*} $info
        assert_match {*usec_min_minute=*} $info
        assert_match {*usec_avg_minute=*} $info
        assert_match {*usec_max_minute=*} $info
        assert_match {*usec_p95_minute=*} $info
        assert_match {*usec_p99_minute=*} $info

        # Aggregation categories
        assert_match {*cmdstat_-:*} $info
        assert_match {*cmdstat_r:*} $info
        assert_match {*cmdstat_w:*} $info
        assert_match {*cmdstat_o:*} $info
    }

    test {INFO commandstats - aggregation category calls sum matches total} {
        r config resetstat
        r set a 1
        r get a
        r set b 2
        r get b
        set info [r info commandstats]

        # Extract calls from aggregation lines
        set all_calls 0
        foreach line [split $info "\r\n"] {
            if {[string match "cmdstat_-:calls=*" $line]} {
                regexp {calls=(\d+)} $line -> all_calls
            }
        }

        # Read + Write + Other should equal All
        set r_calls 0
        set w_calls 0
        set o_calls 0
        foreach line [split $info "\r\n"] {
            if {[string match "cmdstat_r:calls=*" $line]} {
                regexp {calls=(\d+)} $line -> r_calls
            }
            if {[string match "cmdstat_w:calls=*" $line]} {
                regexp {calls=(\d+)} $line -> w_calls
            }
            if {[string match "cmdstat_o:calls=*" $line]} {
                regexp {calls=(\d+)} $line -> o_calls
            }
        }

        set sum_rwo [expr {$r_calls + $w_calls + $o_calls}]
        assert_equal $sum_rwo $all_calls
    }

    test {Command classification: GET is read, SET is write} {
        r config resetstat
        r get a
        r set c 3
        set info [r info commandstats]

        assert_match {*cmdstat_get:calls=1*} $info
        assert_match {*cmdstat_set:calls=1*} $info

        set r_calls 0
        set w_calls 0
        foreach line [split $info "\r\n"] {
            if {[string match "cmdstat_r:calls=*" $line]} {
                regexp {calls=(\d+)} $line -> r_calls
            }
            if {[string match "cmdstat_w:calls=*" $line]} {
                regexp {calls=(\d+)} $line -> w_calls
            }
        }

        # GET should have been counted as read
        assert {$r_calls > 0}
        # SET should have been counted as write
        assert {$w_calls > 0}
    }

    test {CONFIG RESETSTAT resets extended stats} {
        r config resetstat
        set info [r info commandstats]

        # After reset, cmdstat_- should show no calls (except INFO/COMMAND itself)
        # Check that ext_stats data is gone
        foreach line [split $info "\r\n"] {
            if {[string match "cmdstat_get:*" $line] || [string match "cmdstat_set:*" $line]} {
                # These should not appear after reset if no calls
                fail "Command stats should be cleared after resetstat"
            }
        }
    }

    test {CONFIG SET command-latency-tracking no disables extended output} {
        r config set command-latency-tracking yes
        r set d 4
        r get d
        r config set command-latency-tracking no
        set info [r info commandstats]

        # After disabling, no extended fields
        assert_no_match {*usec_min=*} $info
        assert_no_match {*cmdstat_-:*} $info
        assert_no_match {*calls_5s=*} $info
    }

    test {CONFIG GET/SET command-latency-tracking} {
        assert_equal {no} [lindex [r config get command-latency-tracking] 1]

        r config set command-latency-tracking yes
        assert_equal {yes} [lindex [r config get command-latency-tracking] 1]

        r config set command-latency-tracking no
        assert_equal {no} [lindex [r config get command-latency-tracking] 1]
    }

    test {CONFIG GET/SET command-latency-histogram-type} {
        assert_equal {logbucket} [lindex [r config get command-latency-histogram-type] 1]

        # Setting an invalid type should fail
        catch {r config set command-latency-histogram-type hdr} err
        assert_match {*Invalid argument*} $err

        # logbucket should be valid
        r config set command-latency-histogram-type logbucket
        assert_equal {logbucket} [lindex [r config get command-latency-histogram-type] 1]
    }

    test {5-second window decay} {
        r config set command-latency-tracking yes
        r config resetstat
        r set e 5
        set info [r info commandstats]

        # Should see 5s data immediately
        assert_match {*calls_5s=1*} $info

        # Wait 7 seconds for window to rotate out
        after 7000
        set info [r info commandstats]

        # 5s window should be 0 after decay
        assert_match {*calls_5s=0*} $info
    }

    test {Lazy initialization - unused commands have NULL ext_stats} {
        r config set command-latency-tracking yes
        r config resetstat

        # Only call SET and GET
        r set f 6
        r get f

        # INFO shows only commands that were called
        set info [r info commandstats]
        assert_match {*cmdstat_set:*} $info
        assert_match {*cmdstat_get:*} $info

        # Commands like LPUSH, SADD should not appear (never called)
        assert_no_match {*cmdstat_lpush:*} $info
        assert_no_match {*cmdstat_sadd:*} $info
    }

    test {DEBUG SLEEP produces measurable latency values} {
        r config set command-latency-tracking yes
        r config resetstat

        # DEBUG SLEEP 0.01 = 10000 microseconds
        r debug sleep 0.01

        set info [r info commandstats]

        # Find the debug command line
        foreach line [split $info "\r\n"] {
            if {[string match "cmdstat_debug:*" $line]} {
                # usec should be >= 10000 (since we slept 10ms)
                regexp {usec=(\d+)} $line -> usec
                assert {$usec >= 10000}
            }
        }
    }

    test {usec_avg calculation is correct} {
        r config set command-latency-tracking yes
        r config resetstat
        r set g 7
        set info [r info commandstats]

        foreach line [split $info "\r\n"] {
            if {[string match "cmdstat_set:*" $line]} {
                regexp {usec=(\d+)} $line -> usec
                regexp {calls=(\d+)} $line -> calls
                regexp {usec_per_call=([0-9.]+)} $line -> usec_per_call

                # usec_per_call = usec / calls
                set expected [expr {double($usec) / $calls}]
                # Allow small rounding difference
                set diff [expr {abs($usec_per_call - $expected)}]
                assert {$diff < 1.0}
            }
        }
    }

    test {Re-enabling tracking preserves previous data} {
        r config set command-latency-tracking yes
        r config resetstat
        r set h 8
        r get h
        r config set command-latency-tracking no
        r set i 9
        r config set command-latency-tracking yes
        r get h

        set info [r info commandstats]

        # GET should have calls from both before and after re-enabling
        foreach line [split $info "\r\n"] {
            if {[string match "cmdstat_get:*" $line]} {
                regexp {calls=(\d+)} $line -> calls
                # Should be 2 (one before disable, one after re-enable)
                # Plus possibly the first GET before resetstat
                assert {$calls >= 2}
            }
        }
    }
}
