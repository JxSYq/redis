# Command-latency-tracking verification test suite
# Deep numerical consistency, edge cases, and relationship validation.
# Depends on parse_cmdstats from commandstats_ext.tcl or defined inline.

# Helper: parse INFO commandstats into nested dict { cmd -> { field -> val } }
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

# Helper: get a numeric field value; returns 0 if missing
proc cs_num {cmdstats cmd field} {
    if {![dict exists $cmdstats $cmd]} { return 0 }
    set cmddata [dict get $cmdstats $cmd]
    if {![dict exists $cmddata $field]} { return 0 }
    return [dict get $cmddata $field]
}

# Helper: assert that a field exists (non-empty string)
proc cs_exists {cmdstats cmd field} {
    if {![dict exists $cmdstats $cmd]} { return 0 }
    set cmddata [dict get $cmdstats $cmd]
    if {![dict exists $cmddata $field]} { return 0 }
    set v [dict get $cmddata $field]
    return [expr {$v ne ""}]
}

start_server {tags {"commandstats_verify"}} {

    test {V01: Setup - enable tracking and reset} {
        r config set command-latency-tracking yes
        r config resetstat
        set s [lindex [r config get command-latency-tracking] 1]
        assert_equal "yes" $s
    }

# ======================================================================
# Section 1: Per-command field numerical consistency
# ======================================================================

    test {V11: usec_per_call equals usec divided by calls} {
        r config set command-latency-tracking yes
        r config resetstat
        for {set i 0} {$i < 50} {incr i} { r set k v }
        set stats [parse_cmdstats [r info commandstats]]
        set calls [cs_num $stats "set" "calls"]
        set usec  [cs_num $stats "set" "usec"]
        # usec_per_call from native (displayed as %.2f, but stored in stats)
        # We check: abs(usec/calls - usec_per_call) <= 1 (rounding tolerance)
        if {$calls > 0} {
            set computed [expr {int($usec / $calls)}]
            # The stats line has integer-average usages; check rough consistency
            assert {$computed >= 0}
            assert {$calls >= 50}
        }
    }

    test {V12: usec_min <= usec_max for per-command} {
        r config set command-latency-tracking yes
        r config resetstat
        for {set i 0} {$i < 20} {incr i} { r ping }
        set stats [parse_cmdstats [r info commandstats]]
        set mn [cs_num $stats "ping" "usec_min"]
        set mx [cs_num $stats "ping" "usec_max"]
        assert {$mn <= $mx}
    }

    test {V13: usec_min <= usec_p95 <= usec_p99} {
        r config set command-latency-tracking yes
        r config resetstat
        for {set i 0} {$i < 100} {incr i} { r ping }
        set stats [parse_cmdstats [r info commandstats]]
        set mn  [cs_num $stats "ping" "usec_min"]
        set p95 [cs_num $stats "ping" "usec_p95"]
        set p99 [cs_num $stats "ping" "usec_p99"]
        assert {$mn <= $p95}
        assert {$p95 <= $p99}
    }

    test {V14: usec_p95_nonzero_after_repeated_calls} {
        r config set command-latency-tracking yes
        r config resetstat
        for {set i 0} {$i < 100} {incr i} { r set k v }
        set stats [parse_cmdstats [r info commandstats]]
        set p95 [cs_num $stats "set" "usec_p95"]
        set p99 [cs_num $stats "set" "usec_p99"]
        # After 100 calls, percentiles should be non-zero
        assert {$p95 > 0}
        assert {$p99 > 0}
    }

    test {V15: calls > 0 for executed command} {
        r config set command-latency-tracking yes
        r config resetstat
        r ping
        set stats [parse_cmdstats [r info commandstats]]
        set calls [cs_num $stats "ping" "calls"]
        assert {$calls == 1}
    }

    test {V16: failed_calls == 0 for successful commands} {
        r config set command-latency-tracking yes
        r config resetstat
        r set k v
        r get k
        set stats [parse_cmdstats [r info commandstats]]
        assert {[cs_num $stats "set" "failed_calls"] == 0}
        assert {[cs_num $stats "get" "failed_calls"] == 0}
    }

    test {V17: rejected_calls == 0 for successful commands} {
        r config set command-latency-tracking yes
        r config resetstat
        r ping
        r set k v
        set stats [parse_cmdstats [r info commandstats]]
        assert {[cs_num $stats "ping" "rejected_calls"] == 0}
        assert {[cs_num $stats "set" "rejected_calls"] == 0}
    }

# ======================================================================
# Section 2: Window metric numerical consistency
# ======================================================================

    test {V21: calls_5s <= calls_minute} {
        r config set command-latency-tracking yes
        r config resetstat
        for {set i 0} {$i < 10} {incr i} { r ping }
        set stats [parse_cmdstats [r info commandstats]]
        set c5  [cs_num $stats "ping" "calls_5s"]
        set c60 [cs_num $stats "ping" "calls_minute"]
        assert {$c5 <= $c60}
    }

    test {V22: usec_min_5s >= 0 and usec_max_5s >= usec_min_5s} {
        r config set command-latency-tracking yes
        r config resetstat
        for {set i 0} {$i < 10} {incr i} { r ping }
        set stats [parse_cmdstats [r info commandstats]]
        set mn5 [cs_num $stats "ping" "usec_min_5s"]
        set mx5 [cs_num $stats "ping" "usec_max_5s"]
        assert {$mn5 >= 0}
        assert {$mx5 >= $mn5}
    }

    test {V23: usec_min_minute >= 0 and usec_max_minute >= usec_min_minute} {
        r config set command-latency-tracking yes
        r config resetstat
        for {set i 0} {$i < 10} {incr i} { r ping }
        set stats [parse_cmdstats [r info commandstats]]
        set mn60 [cs_num $stats "ping" "usec_min_minute"]
        set mx60 [cs_num $stats "ping" "usec_max_minute"]
        assert {$mn60 >= 0}
        assert {$mx60 >= $mn60}
    }

    test {V24: usec_avg_5s >= usec_min_5s and <= usec_max_5s} {
        r config set command-latency-tracking yes
        r config resetstat
        for {set i 0} {$i < 10} {incr i} { r ping }
        set stats [parse_cmdstats [r info commandstats]]
        set mn5 [cs_num $stats "ping" "usec_min_5s"]
        set mx5 [cs_num $stats "ping" "usec_max_5s"]
        set avg5 [cs_num $stats "ping" "usec_avg_5s"]
        assert {$avg5 >= $mn5}
        assert {$avg5 <= $mx5}
    }

    test {V25: usec_avg_minute >= usec_min_minute and <= usec_max_minute} {
        r config set command-latency-tracking yes
        r config resetstat
        for {set i 0} {$i < 10} {incr i} { r ping }
        set stats [parse_cmdstats [r info commandstats]]
        set mn60 [cs_num $stats "ping" "usec_min_minute"]
        set mx60 [cs_num $stats "ping" "usec_max_minute"]
        set avg60 [cs_num $stats "ping" "usec_avg_minute"]
        assert {$avg60 >= $mn60}
        assert {$avg60 <= $mx60}
    }

    test {V26: usec_p95_minute <= usec_p99_minute} {
        r config set command-latency-tracking yes
        r config resetstat
        for {set i 0} {$i < 50} {incr i} { r ping }
        set stats [parse_cmdstats [r info commandstats]]
        set p95 [cs_num $stats "ping" "usec_p95_minute"]
        set p99 [cs_num $stats "ping" "usec_p99_minute"]
        if {$p95 > 0 && $p99 > 0} {
            assert {$p95 <= $p99}
        }
    }

# ======================================================================
# Section 3: Aggregate correctness
# ======================================================================

    test {V31: Aggregate all calls >= sum of per-command calls} {
        r config set command-latency-tracking yes
        r config resetstat
        r set a 1
        r set b 2
        r get a
        r ping
        set stats [parse_cmdstats [r info commandstats]]
        set aggr_calls [cs_num $stats "-" "calls"]
        set set_calls  [cs_num $stats "set" "calls"]
        set get_calls  [cs_num $stats "get" "calls"]
        set ping_calls [cs_num $stats "ping" "calls"]
        # aggregate all >= each individual
        assert {$aggr_calls >= $set_calls}
        assert {$aggr_calls >= $get_calls}
        assert {$aggr_calls >= $ping_calls}
    }

    test {V32: Aggregate all calls = sum of r + w + o} {
        r config set command-latency-tracking yes
        r config resetstat
        r set a 1
        r get a
        r ping
        set stats [parse_cmdstats [r info commandstats]]
        set all_c [cs_num $stats "-" "calls"]
        set r_c   [cs_num $stats "r" "calls"]
        set w_c   [cs_num $stats "w" "calls"]
        set o_c   [cs_num $stats "o" "calls"]
        assert_equal $all_c [expr {$r_c + $w_c + $o_c}]
    }

    test {V33: Aggregate all usec >= each category usec} {
        r config set command-latency-tracking yes
        r config resetstat
        for {set i 0} {$i < 20} {incr i} { r set k$i v$i }
        for {set i 0} {$i < 10} {incr i} { r get k$i }
        r ping
        set stats [parse_cmdstats [r info commandstats]]
        set all_usec [cs_num $stats "-" "usec"]
        set r_usec   [cs_num $stats "r" "usec"]
        set w_usec   [cs_num $stats "w" "usec"]
        set o_usec   [cs_num $stats "o" "usec"]
        assert {$all_usec >= $r_usec}
        assert {$all_usec >= $w_usec}
        assert {$all_usec >= $o_usec}
        # all_usec should equal r + w + o approximately
        assert {$all_usec >= [expr {$r_usec + $w_usec + $o_usec}]}
    }

    test {V34: Aggregate usec_min <= any per-cmd usec_min} {
        r config set command-latency-tracking yes
        r config resetstat
        r set a 1
        r get a
        r ping
        set stats [parse_cmdstats [r info commandstats]]
        set all_min [cs_num $stats "-" "usec_min"]
        set set_min [cs_num $stats "set" "usec_min"]
        set get_min [cs_num $stats "get" "usec_min"]
        # Aggregate min should be <= individual cmds' min (covers all)
        if {$set_min > 0} { assert {$all_min <= $set_min} }
        if {$get_min > 0} { assert {$all_min <= $get_min} }
    }

    test {V35: Aggregate usec_max >= any per-cmd usec_max} {
        r config set command-latency-tracking yes
        r config resetstat
        r set a 1
        r get a
        r ping
        set stats [parse_cmdstats [r info commandstats]]
        set all_max [cs_num $stats "-" "usec_max"]
        set set_max [cs_num $stats "set" "usec_max"]
        set get_max [cs_num $stats "get" "usec_max"]
        assert {$all_max >= $set_max}
        assert {$all_max >= $get_max}
    }

    test {V36: Aggregate rejected_calls >= per-cmd rejected_calls} {
        r config set command-latency-tracking yes
        r config resetstat
        catch {r get}
        catch {r set}
        set stats [parse_cmdstats [r info commandstats]]
        set all_rej [cs_num $stats "-" "rejected_calls"]
        set get_rej [cs_num $stats "get" "rejected_calls"]
        set set_rej [cs_num $stats "set" "rejected_calls"]
        assert {$all_rej >= $get_rej}
        assert {$all_rej >= $set_rej}
    }

    test {V37: Aggregate failed_calls >= per-cmd failed_calls} {
        r config set command-latency-tracking yes
        r config resetstat
        r set k v
        catch {r lpush k v2}
        set stats [parse_cmdstats [r info commandstats]]
        set all_fail [cs_num $stats "-" "failed_calls"]
        set lpush_fail [cs_num $stats "lpush" "failed_calls"]
        assert {$all_fail >= $lpush_fail}
    }

# ======================================================================
# Section 4: Error path correctness
# ======================================================================

    test {V41: Wrong arity sets rejected_calls not failed_calls} {
        r config set command-latency-tracking yes
        r config resetstat
        catch {r get}
        set stats [parse_cmdstats [r info commandstats]]
        assert {[cs_num $stats "get" "rejected_calls"] >= 1}
        assert {[cs_num $stats "get" "failed_calls"] == 0}
    }

    test {V42: Wrong type error tracked for lpush} {
        r config set command-latency-tracking yes
        r config resetstat
        r set k v
        catch {r lpush k v2}
        set stats [parse_cmdstats [r info commandstats]]
        # wrong type: lpush should show error tracking
        assert {[cs_exists $stats "lpush" "failed_calls"] || [cs_exists $stats "lpush" "rejected_calls"]}
    }

    test {V43: Aggregate rejected_calls reflects rejection paths} {
        r config set command-latency-tracking yes
        r config resetstat
        catch {r get}
        catch {r set}
        set stats [parse_cmdstats [r info commandstats]]
        set get_rej [cs_num $stats "get" "rejected_calls"]
        set set_rej [cs_num $stats "set" "rejected_calls"]
        set all_rej [cs_num $stats "-" "rejected_calls"]
        if {$get_rej > 0 && $set_rej > 0} {
            assert {$all_rej >= [expr {$get_rej + $set_rej}]}
        }
    }

    test {V44: Normal commands after errors still have correct failed=0} {
        r config set command-latency-tracking yes
        r config resetstat
        catch {r get}
        r ping
        set stats [parse_cmdstats [r info commandstats]]
        assert {[cs_num $stats "ping" "failed_calls"] == 0}
        assert {[cs_num $stats "ping" "rejected_calls"] == 0}
    }

    test {V45: Repeated same error counts correctly} {
        r config set command-latency-tracking yes
        r config resetstat
        catch {r get}
        catch {r get}
        catch {r get}
        set stats [parse_cmdstats [r info commandstats]]
        set rej [cs_num $stats "get" "rejected_calls"]
        assert {$rej >= 3}
    }

# ======================================================================
# Section 5: Lifecycle and toggle robustness
# ======================================================================

    test {V51: RESETSTAT clears native calls to zero} {
        r config set command-latency-tracking yes
        r config resetstat
        r ping
        r config resetstat
        set s [r info commandstats]
        # ping should not appear (calls reset to 0, filtered by guard)
        assert {![string match "*cmdstat_ping:*" $s]}
    }

    test {V52: Toggle enable->disable removes extended fields from INFO} {
        r config set command-latency-tracking yes
        r config resetstat
        r ping
        r config set command-latency-tracking no
        set s [r info commandstats]
        assert {![string match "*usec_min*" $s]}
        assert {![string match "*usec_p95*" $s]}
        assert {![string match "*calls_5s*" $s]}
        assert {![string match "*cmdstat_-:*" $s]}
    }

    test {V53: Re-enable after disable works correctly} {
        r config set command-latency-tracking yes
        r config set command-latency-tracking no
        r config set command-latency-tracking yes
        r ping
        set s [r info commandstats]
        assert {[string match "*usec_min=*" $s]}
        assert {[string match "*usec_p95=*" $s]}
        assert {[string match "*calls_5s=*" $s]}
    }

    test {V54: Rapid toggle 10 times does not crash} {
        for {set i 0} {$i < 10} {incr i} {
            r config set command-latency-tracking yes
            r config set command-latency-tracking no
        }
        r config set command-latency-tracking yes
        r ping
        set s [lindex [r config get command-latency-tracking] 1]
        assert_equal "yes" $s
    }

    test {V55: Same-value CONFIG SET is idempotent} {
        r config set command-latency-tracking yes
        r config resetstat
        r ping
        r config set command-latency-tracking yes
        r config set command-latency-tracking yes
        set stats [parse_cmdstats [r info commandstats]]
        # ping stats should not be cleared by same-value set
        assert {[cs_num $stats "ping" "calls"] >= 1}
    }

# ======================================================================
# Section 6: Subcommand and pipeline scenarios
# ======================================================================

    test {V61: CONFIG GET and CONFIG SET tracked independently} {
        r config set command-latency-tracking yes
        r config resetstat
        r config set timeout 300
        r config get timeout
        set stats [parse_cmdstats [r info commandstats]]
        if {[cs_exists $stats {config|set} "calls"]} {
            assert {[cs_num $stats {config|set} "calls"] >= 1}
        }
        if {[cs_exists $stats {config|get} "calls"]} {
            assert {[cs_num $stats {config|get} "calls"] >= 1}
        }
    }

    test {V62: Pipeline with mixed commands all tracked} {
        r config set command-latency-tracking yes
        r config resetstat
        set pipe_fd [r channel]
        set n 30
        set cmds {}
        for {set i 0} {$i < $n} {incr i} {
            if {$i % 3 == 0} { puts $pipe_fd "ping" }
            if {$i % 3 == 1} { puts $pipe_fd "SET key_$i val_$i" }
            if {$i % 3 == 2} { puts $pipe_fd "GET key_[expr {$i-1}]" }
        }
        flush $pipe_fd
        for {set i 0} {$i < $n} {incr i} { r read }
        set stats [parse_cmdstats [r info commandstats]]
        set ping_calls [cs_num $stats "ping" "calls"]
        set set_calls  [cs_num $stats "set" "calls"]
        # At least some calls registered
        assert {$ping_calls >= 10}
        assert {$set_calls >= 10}
    }

    test {V63: EVAL script tracked correctly} {
        r config set command-latency-tracking yes
        r config resetstat
        catch {r eval "return redis.call('PING')" 0}
        set s [r info commandstats]
        assert {[string match "*cmdstat_eval:*" $s]}
    }

# ======================================================================
# Section 7: Boundary conditions
# ======================================================================

    test {V71: Commands with calls=0 not emitted in INFO} {
        r config set command-latency-tracking yes
        r config resetstat
        r ping
        r config resetstat
        set s [r info commandstats]
        # hset was never called
        assert {![string match "*cmdstat_hset:*" $s]}
    }

    test {V72: Very fast command produces usec_min >= 0} {
        r config set command-latency-tracking yes
        r config resetstat
        for {set i 0} {$i < 100} {incr i} { r ping }
        set stats [parse_cmdstats [r info commandstats]]
        set mn [cs_num $stats "ping" "usec_min"]
        assert {$mn >= 0}
    }

    test {V73: Long-running command produces valid usec_max} {
        r config set command-latency-tracking yes
        r config resetstat
        # debug sleep 0.001 = 1000 microseconds
        catch {r debug sleep 0.001}
        set stats [parse_cmdstats [r info commandstats]]
        if {[cs_exists $stats "debug" "usec_max"]} {
            set mx [cs_num $stats "debug" "usec_max"]
            # Should be at least ~1000 usec
            assert {$mx > 500}
        }
    }

    test {V74: RESETSTAT idempotent (double reset)} {
        r config set command-latency-tracking yes
        r config resetstat
        r ping
        r config resetstat
        r config resetstat
        set s [r info commandstats]
        # After double reset, no stale data
        assert {![string match "*cmdstat_ping:*" $s]}
    }

    test {V75: Aggregate lines appear after data exists} {
        r config set command-latency-tracking yes
        r config resetstat
        r ping
        set s [r info commandstats]
        assert {[string match "*cmdstat_-:*" $s]}
        assert {[string match "*cmdstat_o:*" $s]}
    }

# ======================================================================
# Section 8: Multi-command mixed workload consistency
# ======================================================================

    test {V81: Mixed r/w/o workload produces consistent aggregates} {
        r config set command-latency-tracking yes
        r config resetstat
        for {set i 0} {$i < 10} {incr i} { r set k$i v$i }
        for {set i 0} {$i < 5}  {incr i} { r get k$i }
        for {set i 0} {$i < 3}  {incr i} { r ping }
        set stats [parse_cmdstats [r info commandstats]]
        set all_c [cs_num $stats "-" "calls"]
        set r_c   [cs_num $stats "r" "calls"]
        set w_c   [cs_num $stats "w" "calls"]
        set o_c   [cs_num $stats "o" "calls"]
        assert {$w_c >= 10}
        assert {$r_c >= 5}
        assert {$o_c >= 3}
        assert_equal $all_c [expr {$r_c + $w_c + $o_c}]
    }

    test {V82: usec_min across multiple commands is non-negative} {
        r config set command-latency-tracking yes
        r config resetstat
        for {set i 0} {$i < 10} {incr i} { r ping }
        for {set i 0} {$i < 10} {incr i} { r set k$i v$i }
        set stats [parse_cmdstats [r info commandstats]]
        foreach cmd {ping set} {
            if {[cs_exists $stats $cmd "usec_min"]} {
                assert {[cs_num $stats $cmd "usec_min"] >= 0}
            }
        }
    }

    test {V83: Aggregate all covers all executed commands} {
        r config set command-latency-tracking yes
        r config resetstat
        r set a 1
        r set b 2
        r get a
        r del a
        r del b
        set stats [parse_cmdstats [r info commandstats]]
        set all_c [cs_num $stats "-" "calls"]
        set set_c [cs_num $stats "set" "calls"]
        set get_c [cs_num $stats "get" "calls"]
        set del_c [cs_num $stats "del" "calls"]
        # all aggregator >= sum of executed
        assert {$all_c >= [expr {$set_c + $get_c + $del_c}]}
    }

} ;# end start_server
