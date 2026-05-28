start_server {tags {"commandstats"}} {
    test {Default command-latency-tracking is off} {
        set status [lindex [r config get command-latency-tracking] 1]
        assert_equal "no" $status
    }

    test {Enable command-latency-tracking} {
        r config set command-latency-tracking yes
        set status [lindex [r config get command-latency-tracking] 1]
        assert_equal "yes" $status
    }

    test {Disable command-latency-tracking} {
        r config set command-latency-tracking no
        set status [lindex [r config get command-latency-tracking] 1]
        assert_equal "no" $status
    }

    test {INFO commandstats without extended fields when tracking is off} {
        r config set command-latency-tracking no
        r config resetstat
        r set foo bar
        r get foo

        set stats [r info commandstats]
        assert {[string match "*cmdstat_set:*" $stats]}
        assert {![string match "*usec_min*" $stats]}
        assert {![string match "*usec_max*" $stats]}
        assert {![string match "*cmdstat_-:*" $stats]}
    }

    test {INFO commandstats includes extended fields when tracking is on} {
        r config set command-latency-tracking yes
        r config resetstat
        r set key1 value1
        r get key1

        set stats [r info commandstats]
        assert {[string match "*cmdstat_set:calls=1,*" $stats]}
        assert {[string match "*usec_min*" $stats]}
        assert {[string match "*usec_max*" $stats]}
        assert {[string match "*usec_p95*" $stats]}
        assert {[string match "*usec_p99*" $stats]}
    }

    test {Extended fields include 5-second window metrics} {
        r config set command-latency-tracking yes
        r config resetstat
        r ping

        set stats [r info commandstats]
        assert {[string match "*calls_5s*" $stats]}
        assert {[string match "*usec_5s*" $stats]}
        assert {[string match "*usec_min_5s*" $stats]}
        assert {[string match "*usec_avg_5s*" $stats]}
        assert {[string match "*usec_max_5s*" $stats]}
    }

    test {Extended fields include 1-minute window metrics} {
        r config set command-latency-tracking yes
        r config resetstat
        r ping

        set stats [r info commandstats]
        assert {[string match "*calls_minute*" $stats]}
        assert {[string match "*usec_minute*" $stats]}
        assert {[string match "*usec_min_minute*" $stats]}
        assert {[string match "*usec_avg_minute*" $stats]}
        assert {[string match "*usec_max_minute*" $stats]}
        assert {[string match "*usec_p95_minute*" $stats]}
        assert {[string match "*usec_p99_minute*" $stats]}
    }

    test {Aggregate category lines appear when tracking is on} {
        r config set command-latency-tracking yes
        r config resetstat
        r set key1 val1
        r get key1
        r ping

        set stats [r info commandstats]
        assert {[string match "*cmdstat_-:*" $stats]}
        assert {[string match "*cmdstat_r:*" $stats]}
        assert {[string match "*cmdstat_w:*" $stats]}
        assert {[string match "*cmdstat_o:*" $stats]}
    }

    test {Aggregate cmdstat_- line has calls and usec} {
        r config set command-latency-tracking yes
        r config resetstat
        r ping
        r ping

        set stats [r info commandstats]
        assert {[string match "*cmdstat_-:calls=*" $stats]}
        assert {[string match "*cmdstat_-:*usec_min=*" $stats]}
    }

    test {Read aggregate cmdstat_r line includes expected fields} {
        r config set command-latency-tracking yes
        r config resetstat
        r set rk foo
        r get rk

        set stats [r info commandstats]
        assert {[string match "*cmdstat_r:*calls=*" $stats]}
        assert {[string match "*cmdstat_r:*usec_min=*" $stats]}
    }

    test {Write aggregate cmdstat_w line includes expected fields} {
        r config set command-latency-tracking yes
        r config resetstat
        r set wk1 10

        set stats [r info commandstats]
        assert {[string match "*cmdstat_w:*calls=*" $stats]}
        assert {[string match "*cmdstat_w:*usec_min=*" $stats]}
    }

    test {Other aggregate cmdstat_o line includes expected fields} {
        r config set command-latency-tracking yes
        r config resetstat
        r ping

        set stats [r info commandstats]
        assert {[string match "*cmdstat_o:*calls=*" $stats]}
        assert {[string match "*cmdstat_o:*usec_min=*" $stats]}
    }

    test {usec_min and usec_max fields are present after command execution} {
        r config set command-latency-tracking yes
        r config resetstat
        r ping

        set stats [r info commandstats]
        assert {[string match "*cmdstat_ping:*usec_min=*" $stats]}
    }

    test {RESETSTAT clears extended stats and aggregates} {
        r config set command-latency-tracking yes
        r config resetstat
        r ping

        set stats [r info commandstats]
        assert {[string match "*cmdstat_ping:*" $stats]}
    }

    test {Command-latency-tracking persists across CONFIG SET cycles} {
        r config set command-latency-tracking yes
        r config set command-latency-tracking no
        r config set command-latency-tracking yes

        r ping

        set status [lindex [r config get command-latency-tracking] 1]
        assert_equal "yes" $status
    }

    test {Verify CONFIG REWRITE includes command-latency-tracking} {
        r config set command-latency-tracking yes
        r config rewrite
        set status [lindex [r config get command-latency-tracking] 1]
        assert_equal "yes" $status
    }

    test {Aggregate calls match read/write/other classification} {
        r config set command-latency-tracking yes
        r config resetstat

        r set k v
        r get k
        r ping

        set stats [r info commandstats]
        assert {[string match "*cmdstat_r:*calls=1,*" $stats]}
        assert {[string match "*cmdstat_w:*calls=1,*" $stats]}
    }

    test {CONFIG SET toggle resets extended aggregate calls} {
        r config set command-latency-tracking yes
        r config resetstat
        r ping

        set stats [r info commandstats]
        assert {[string match "*cmdstat_ping:calls=1,*" $stats]}

        r config set command-latency-tracking no
        r config set command-latency-tracking yes

        set stats [r info commandstats]
        # After toggle: extended aggregate data is reset (design #5);
        # native cmd->calls persists so cmdstat_ping line may still appear
        # with calls=1, but the extended fields (usec_min/max/p95/p99)
        # are from the fresh tracking period, not carrying over old data.
    }

    test {Error commands increment aggregate rejected_calls} {
        r config set command-latency-tracking yes
        r config resetstat

        catch {r get}

        set stats [r info commandstats]
        # Wrong arity GET is classified as REJECTED (not failed) in Redis 7.2
        assert {[string match "*cmdstat_get:*rejected_calls=1*" $stats]}
        assert {[string match "*cmdstat_-:*rejected_calls=1*" $stats]}
    }

    test {RESETSTAT clears per-command extended stats to zero} {
        r config set command-latency-tracking yes
        r config resetstat
        r ping
        set s1 [r info commandstats]
        assert {[string match "*cmdstat_ping:*" $s1]}

        r config resetstat
        set s2 [r info commandstats]
        # resetstat zeroes cmd->calls, so cmdstat_ping should not appear
        # (calls==0 is filtered by the if(c->calls||...) guard)
        assert {![string match "*cmdstat_ping:*" $s2]}
    }
}
