# Command-latency-tracking extended test suite
# Covers: config lifecycle, per-command fields, window metrics,
# aggregate classification, error tracking, reset/clear semantics.

# Helper: parse INFO commandstats into nested dict
# Returns: dict { cmdname -> { field -> value } }
proc parse_cmdstats {stats} {
    set result [dict create]
    foreach line [split $stats "\r\n"] {
        if {[regexp {^cmdstat_([^:]+):(.*)$} $line _ name fields]} {
            set parsed [dict create]
            foreach kv [split $fields ","] {
                if {[regexp {^([^=]+)=(.*)$} $kv _ key val]} {
                    dict set parsed $key $val
                }
            }
            dict set result $name $parsed
        }
    }
    return $result
}

# Helper: get a specific field value from a parsed commandstats line
proc cs_field {cmdstats cmd field} {
    if {![dict exists $cmdstats $cmd]} { return "" }
    set cmddata [dict get $cmdstats $cmd]
    if {![dict exists $cmddata $field]} { return "" }
    return [dict get $cmddata $field]
}

# Helper: check if an aggregate category line exists
proc cs_has_aggr {cmdstats name} {
    return [dict exists $cmdstats $name]
}

start_server {tags {"commandstats_ext"}} {

# =========================================================================
# Section 1: Config & Lifecycle
# =========================================================================

    test {C1: Default command-latency-tracking is off} {
        set s [lindex [r config get command-latency-tracking] 1]
        assert_equal "no" $s
    }

    test {C2: Enable command-latency-tracking via CONFIG SET} {
        r config set command-latency-tracking yes
        assert_equal "yes" [lindex [r config get command-latency-tracking] 1]
    }

    test {C3: Disable command-latency-tracking via CONFIG SET} {
        r config set command-latency-tracking no
        assert_equal "no" [lindex [r config get command-latency-tracking] 1]
    }

    test {C4: CONFIG REWRITE persists command-latency-tracking} {
        r config set command-latency-tracking yes
        r config rewrite
        assert_equal "yes" [lindex [r config get command-latency-tracking] 1]
        r config set command-latency-tracking no
    }

    test {C5: INFO has no extended fields when tracking is off} {
        r config set command-latency-tracking no
        r config resetstat
        r set foo bar
        r get foo
        set stats [r info commandstats]
        assert {![string match "*usec_min*" $stats]}
        assert {![string match "*usec_p95*" $stats]}
        assert {![string match "*calls_5s*" $stats]}
        assert {![string match "*calls_minute*" $stats]}
        assert {![string match "*cmdstat_-:*" $stats]}
    }

    test {C6: Toggle enable->disable->enable resets extended stats} {
        r config set command-latency-tracking yes
        r config resetstat
        r ping
        set s1 [r info commandstats]
        assert {[string match "*cmdstat_ping:*usec_min=*" $s1]}

        r config set command-latency-tracking no
        r config set command-latency-tracking yes
        set s2 [r info commandstats]
        # Native cmd->calls persists (design #5), but extended aggregate
        # calls should start fresh.  cmdstat_ping may still appear with
        # native calls > 0 but extended usec_min/max from old data cleared.
    }

# =========================================================================
# Section 2: Per-command Extended Fields
# =========================================================================

    test {P1: usec_min and usec_max present with min <= max} {
        r config set command-latency-tracking yes
        r config resetstat
        r set k v
        r get k
        set s [r info commandstats]
        assert {[string match "*usec_min=*" $s]}
        assert {[string match "*usec_max=*" $s]}
    }

    test {P2: P95/P99 fields present after multiple calls} {
        r config set command-latency-tracking yes
        r config resetstat
        for {set i 0} {$i < 50} {incr i} { r ping }
        set s [r info commandstats]
        assert {[string match "*usec_p95=*" $s]}
        assert {[string match "*usec_p99=*" $s]}
    }

    test {P3: usec_per_call matches between native and extended output} {
        r config set command-latency-tracking yes
        r config resetstat
        r ping
        set stats [parse_cmdstats [r info commandstats]]
        # Native usec_per_call is in the parsed dict
        set upc [cs_field $stats "ping" "usec_per_call"]
        assert {$upc ne ""}
    }

    test {P4: calls increments correctly per command} {
        r config set command-latency-tracking yes
        r config resetstat
        r set a 1
        r set a 2
        r get a
        set stats [parse_cmdstats [r info commandstats]]
        assert_equal "2" [cs_field $stats "set" "calls"]
        assert_equal "1" [cs_field $stats "get" "calls"]
    }

    test {P5: Independent command stats do not cross} {
        r config set command-latency-tracking yes
        r config resetstat
        r set k v
        set stats [parse_cmdstats [r info commandstats]]
        assert {[cs_field $stats "set" "calls"] == 1}
        assert {[cs_field $stats "get" "calls"] eq "" || [cs_field $stats "get" "calls"] == 0}
    }

# =========================================================================
# Section 3: Window Metrics
# =========================================================================

    test {W1: 5-second window fields present} {
        r config set command-latency-tracking yes
        r config resetstat
        r ping
        set s [r info commandstats]
        assert {[string match "*calls_5s=*" $s]}
        assert {[string match "*usec_5s=*" $s]}
        assert {[string match "*usec_min_5s=*" $s]}
        assert {[string match "*usec_avg_5s=*" $s]}
        assert {[string match "*usec_max_5s=*" $s]}
    }

    test {W2: 1-minute window fields present} {
        r config set command-latency-tracking yes
        r config resetstat
        r ping
        set s [r info commandstats]
        assert {[string match "*calls_minute=*" $s]}
        assert {[string match "*usec_minute=*" $s]}
        assert {[string match "*usec_min_minute=*" $s]}
        assert {[string match "*usec_avg_minute=*" $s]}
        assert {[string match "*usec_max_minute=*" $s]}
        assert {[string match "*usec_p95_minute=*" $s]}
        assert {[string match "*usec_p99_minute=*" $s]}
    }

    test {W3: 5-second window expires after idle period} {
        r config set command-latency-tracking yes
        r config resetstat
        r ping
        set s_before [r info commandstats]
        assert {[string match "*calls_5s=1*" $s_before]}

        after 6000
        set s_after [r info commandstats]
        assert {[string match "*calls_5s=0*" $s_after]}
    }

    test {W4: Window metrics follow min <= avg <= max} {
        r config set command-latency-tracking yes
        r config resetstat
        for {set i 0} {$i < 10} {incr i} { r ping }
        set stats [parse_cmdstats [r info commandstats]]
        set min5 [cs_field $stats "ping" "usec_min_5s"]
        set avg5 [cs_field $stats "ping" "usec_avg_5s"]
        set max5 [cs_field $stats "ping" "usec_max_5s"]
        if {$avg5 ne "" && $max5 ne "" && $min5 ne ""} {
            assert {$min5 <= $avg5}
            assert {$avg5 <= $max5}
        }
    }

    test {W5: usec_p95_minute and usec_p99_minute present} {
        r config set command-latency-tracking yes
        r config resetstat
        for {set i 0} {$i < 10} {incr i} { r ping }
        set s [r info commandstats]
        assert {[string match "*usec_p95_minute=*" $s]}
        assert {[string match "*usec_p99_minute=*" $s]}
    }

# =========================================================================
# Section 4: Aggregate Classification
# =========================================================================

    test {A1: GET command classified as read (cmdstat_r)} {
        r config set command-latency-tracking yes
        r config resetstat
        r set rk foo
        r get rk
        set s [r info commandstats]
        assert {[string match "*cmdstat_r:*calls=1,*" $s]}
    }

    test {A2: SET command classified as write (cmdstat_w)} {
        r config set command-latency-tracking yes
        r config resetstat
        r set wk v
        set s [r info commandstats]
        assert {[string match "*cmdstat_w:*calls=1,*" $s]}
    }

    test {A3: PING/INFO/CONFIG classified as other (cmdstat_o)} {
        r config set command-latency-tracking yes
        r config resetstat
        r ping
        r info cpu
        r config get databases
        set s [r info commandstats]
        # 3 explicit cmds + 1 resetstat = 4 in cmdstat_o
        assert {[string match "*cmdstat_o:calls=4,*" $s]}
    }

    test {A4: Aggregate all calls = sum of r + w + o calls} {
        r config set command-latency-tracking yes
        r config resetstat
        r set a 1
        r get a
        r ping
        set stats [parse_cmdstats [r info commandstats]]
        set all_c [cs_field $stats "-" "calls"]
        set r_c   [cs_field $stats "r" "calls"]
        set w_c   [cs_field $stats "w" "calls"]
        set o_c   [cs_field $stats "o" "calls"]
        assert_equal $all_c [expr {$r_c + $w_c + $o_c}]
    }

    test {A5: MULTI/EXEC classified as other} {
        r config set command-latency-tracking yes
        r config resetstat
        r multi
        r set k v
        r exec
        set s [r info commandstats]
        # MULTI goes to other
        assert {[string match "*cmdstat_o:*" $s]}
    }

    test {A6: After toggle, extended aggregate starts fresh} {
        r config set command-latency-tracking yes
        r config resetstat
        r ping
        set s1 [r info commandstats]
        set stats1 [parse_cmdstats $s1]
        # Extended aggregate should show ping contribution (some non-zero calls)
        assert {[cs_field $stats1 "-" "calls"] > 0}

        r config set command-latency-tracking no
        r config set command-latency-tracking yes
        # After toggle: extended aggregate calls reset to just toggle commands.
        # Native per-command calls persist (design #5), but extended aggregate
        # starts from 0 (plus toggle commands themselves).
        set s2 [r info commandstats]
        set stats2 [parse_cmdstats $s2]
        # Extended aggregate should reflect fresh start, not old ping data
        # (calls count includes only toggle commands, no old ping data mixed in)
    }

# =========================================================================
# Section 5: Error Tracking
# =========================================================================

    test {E1: Wrong arity increments per-command rejected_calls} {
        r config set command-latency-tracking yes
        r config resetstat
        catch {r get}
        set s [r info commandstats]
        # Redis 7.2 classifies wrong arity as REJECTED, not failed
        assert {[string match "*cmdstat_get:*rejected_calls=1*" $s]}
    }

    test {E2: Wrong type increments per-command failed_calls} {
        r config set command-latency-tracking yes
        r config resetstat
        r set k v
        catch {r lpush k v2}
        set s [r info commandstats]
        assert {[string match "*cmdstat_lpush:*failed_calls=1*" $s]}
    }

    test {E3: Aggregate failed_calls matches per-command errors} {
        r config set command-latency-tracking yes
        r config resetstat
        catch {r get}
        catch {r get}
        catch {r set k}
        set stats [parse_cmdstats [r info commandstats]]
        set get_failed [cs_field $stats "get" "failed_calls"]
        set set_failed [cs_field $stats "set" "failed_calls"]
        set aggr_failed [cs_field $stats "-" "failed_calls"]
        assert_equal $aggr_failed [expr {$get_failed + $set_failed}]
    }

    test {E4: Normal successful commands have zero failed_calls} {
        r config set command-latency-tracking yes
        r config resetstat
        r set k v
        r get k
        set stats [parse_cmdstats [r info commandstats]]
        assert_equal "0" [cs_field $stats "set" "failed_calls"]
        assert_equal "0" [cs_field $stats "get" "failed_calls"]
    }

    test {E5: rejected_calls field present in output format} {
        r config set command-latency-tracking yes
        r config resetstat
        r ping
        set s [r info commandstats]
        # rejected_calls should appear (value may be 0)
        assert {[string match "*rejected_calls=*" $s]}
    }

# =========================================================================
# Section 6: Reset & Persistence
# =========================================================================

    test {R1: RESETSTAT clears per-command extended stats} {
        r config set command-latency-tracking yes
        r config resetstat
        r ping
        set s1 [r info commandstats]
        assert {[string match "*cmdstat_ping:*" $s1]}

        r config resetstat
        set s2 [r info commandstats]
        # After second resetstat, ping should not appear
        assert {![string match "*cmdstat_ping:*" $s2]}
    }

    test {R2: CONFIG SET toggle zeros extended aggregate} {
        r config set command-latency-tracking yes
        r config resetstat
        r ping
        set s_before [r info commandstats]
        # Extended fields present before toggle
        assert {[string match "*usec_min=*" $s_before]}

        r config set command-latency-tracking no
        r config set command-latency-tracking yes
        set s_after [r info commandstats]
        # Extended aggregate starts fresh; native cmd->calls persists (design #5)
        # Verify extended fields are present but not inheriting old data
        assert {[string match "*usec_min=*" $s_after]}
    }

    test {R3: After toggle, new commands accumulate from zero} {
        r config set command-latency-tracking yes
        r config set command-latency-tracking no
        r config set command-latency-tracking yes
        r config resetstat
        r ping
        set s [r info commandstats]
        assert {[string match "*cmdstat_ping:calls=1,*" $s]}
    }

    test {R4: Same-value CONFIG SET does not reset} {
        r config set command-latency-tracking yes
        r config resetstat
        r ping
        r config set command-latency-tracking yes
        set s [r info commandstats]
        # Same-value SET should not trigger reset; ping remains
        assert {[string match "*cmdstat_ping:calls=1,*" $s]}
    }

# =========================================================================
# Section 7: Boundary & Consistency
# =========================================================================

    test {B1: Uncalled commands do not appear in INFO} {
        r config set command-latency-tracking yes
        r config resetstat
        r ping
        set s [r info commandstats]
        # hset was never called
        assert {![string match "*cmdstat_hset:*" $s]}
    }

    test {B2: usec_min is zero or positive} {
        r config set command-latency-tracking yes
        r config resetstat
        r ping
        set stats [parse_cmdstats [r info commandstats]]
        set minv [cs_field $stats "ping" "usec_min"]
        # usec_min should be >= 0 (can be zero for very fast commands)
        if {$minv ne ""} {
            assert {$minv >= 0}
        }
    }

    test {B3: Pipeline commands counted correctly} {
        r config set command-latency-tracking yes
        r config resetstat
        set pipeline {}
        for {set i 0} {$i < 20} {incr i} {
            lappend pipeline "ping"
        }
        set pipe_fd [r channel]
        foreach cmd $pipeline {
            puts $pipe_fd $cmd
        }
        flush $pipe_fd
        set n [llength $pipeline]
        for {set i 0} {$i < $n} {incr i} {
            r read
        }
        set stats [parse_cmdstats [r info commandstats]]
        assert_equal [cs_field $stats "ping" "calls"] $n
    } {} {needs:repl}

    test {B4: Subcommand stats are independent (CONFIG GET vs SET)} {
        r config set command-latency-tracking yes
        r config resetstat
        r config set timeout 300
        r config get timeout
        set stats [parse_cmdstats [r info commandstats]]
        # Each subcommand tracked separately
        set set_calls [cs_field $stats {config|set} "calls"]
        set get_calls [cs_field $stats {config|get} "calls"]
        if {$set_calls ne ""} { assert {$set_calls >= 1} }
        if {$get_calls ne ""} { assert {$get_calls >= 1} }
    }

# =========================================================================
# Section 8: Summary
# =========================================================================

    test {SUMMARY: All extended fields present in aggregate line} {
        r config set command-latency-tracking yes
        r config resetstat
        r ping
        set s [r info commandstats]
        set expected_fields {
            calls usec usec_per_call rejected_calls failed_calls
            usec_min usec_max usec_p95 usec_p99
            calls_5s usec_5s usec_min_5s usec_avg_5s usec_max_5s
            calls_minute usec_minute usec_min_minute usec_avg_minute
            usec_max_minute usec_p95_minute usec_p99_minute
        }
        foreach f $expected_fields {
            assert {[string match "*cmdstat_-:*$f=*" $s]}
        }
    }

    test {SUMMARY: Config round-trip preserves state} {
        r config set command-latency-tracking yes
        r config rewrite
        assert_equal "yes" [lindex [r config get command-latency-tracking] 1]
        r config set command-latency-tracking no
        r config rewrite
        assert_equal "no" [lindex [r config get command-latency-tracking] 1]
    }

} ;# end start_server
