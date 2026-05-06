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
}

start_server {tags {"audit-log"}} {
    test {DEBUG AUDIT-TYPE: string commands} {
        assert_equal "string" [r debug audit-type SET]
        assert_equal "string" [r debug audit-type GET]
        assert_equal "string" [r debug audit-type MSET]
    }

    test {DEBUG AUDIT-TYPE: hash commands} {
        assert_equal "hash" [r debug audit-type HGET]
        assert_equal "hash" [r debug audit-type HSET]
        assert_equal "hash" [r debug audit-type HDEL]
    }

    test {DEBUG AUDIT-TYPE: list commands} {
        assert_equal "list" [r debug audit-type LPUSH]
        assert_equal "list" [r debug audit-type RPOP]
        assert_equal "list" [r debug audit-type BLPOP]
    }

    test {DEBUG AUDIT-TYPE: set commands} {
        assert_equal "set" [r debug audit-type SADD]
        assert_equal "set" [r debug audit-type SMEMBERS]
    }

    test {DEBUG AUDIT-TYPE: sorted-set commands} {
        assert_equal "sorted-set" [r debug audit-type ZADD]
        assert_equal "sorted-set" [r debug audit-type ZRANGE]
    }

    test {DEBUG AUDIT-TYPE: bitmap commands} {
        assert_equal "bitmap" [r debug audit-type BITCOUNT]
        assert_equal "bitmap" [r debug audit-type SETBIT]
    }

    test {DEBUG AUDIT-TYPE: hyperloglog commands} {
        assert_equal "hyperloglog" [r debug audit-type PFADD]
        assert_equal "hyperloglog" [r debug audit-type PFCOUNT]
    }

    test {DEBUG AUDIT-TYPE: geo commands} {
        assert_equal "geo" [r debug audit-type GEOADD]
        assert_equal "geo" [r debug audit-type GEODIST]
    }

    test {DEBUG AUDIT-TYPE: stream commands} {
        assert_equal "stream" [r debug audit-type XADD]
        assert_equal "stream" [r debug audit-type XREAD]
    }

    test {DEBUG AUDIT-TYPE: pubsub commands} {
        assert_equal "pubsub" [r debug audit-type PUBLISH]
        assert_equal "pubsub" [r debug audit-type SUBSCRIBE]
    }

    test {DEBUG AUDIT-TYPE: scripting commands} {
        assert_equal "scripting" [r debug audit-type EVAL]
        assert_equal "scripting" [r debug audit-type EVALSHA]
    }

    test {DEBUG AUDIT-TYPE: transactions commands} {
        assert_equal "transactions" [r debug audit-type MULTI]
        assert_equal "transactions" [r debug audit-type EXEC]
    }

    test {DEBUG AUDIT-TYPE: connection commands} {
        assert_equal "connection" [r debug audit-type PING]
        assert_equal "connection" [r debug audit-type SELECT]
    }

    test {DEBUG AUDIT-TYPE: server commands} {
        assert_equal "server" [r debug audit-type INFO]
        assert_equal "server" [r debug audit-type CONFIG]
    }

    test {DEBUG AUDIT-TYPE: generic commands} {
        assert_equal "generic" [r debug audit-type DEL]
        assert_equal "generic" [r debug audit-type EXISTS]
    }

    test {DEBUG AUDIT-TYPE: cluster commands} {
        assert_equal "cluster" [r debug audit-type CLUSTER]
    }

    test {DEBUG AUDIT-TYPE: module commands (bf/cf/json/search/timeseries/topk)} {
        assert_equal "bf" [r debug audit-type BF.ADD]
        assert_equal "cf" [r debug audit-type CF.ADD]
        assert_equal "json" [r debug audit-type JSON.SET]
        assert_equal "search" [r debug audit-type FT.SEARCH]
        assert_equal "timeseries" [r debug audit-type TS.ADD]
        assert_equal "topk" [r debug audit-type TOPK.ADD]
    }

    test {DEBUG AUDIT-TYPE: case insensitive lookup} {
        assert_equal "string" [r debug audit-type set]
        assert_equal "string" [r debug audit-type Set]
        assert_equal "hash" [r debug audit-type hget]
        assert_equal "hash" [r debug audit-type Hget]
        assert_equal "sorted-set" [r debug audit-type zadd]
    }

    test {DEBUG AUDIT-TYPE: unknown command returns undefined} {
        assert_equal "undefined" [r debug audit-type UNKNOWNCMD]
        assert_equal "undefined" [r debug audit-type NOTACMD]
    }
}

start_server {tags {"audit-log"}} {
    test {DEBUG AUDIT-KEYS: single key command (SET)} {
        set keys [r debug audit-keys SET mykey myvalue]
        assert_equal 1 [llength $keys]
        assert_equal "mykey" [lindex $keys 0]
    }

    test {DEBUG AUDIT-KEYS: multi-key command (DEL)} {
        set keys [r debug audit-keys DEL k1 k2 k3]
        assert_equal 3 [llength $keys]
        assert_equal "k1" [lindex $keys 0]
        assert_equal "k2" [lindex $keys 1]
        assert_equal "k3" [lindex $keys 2]
    }

    test {DEBUG AUDIT-KEYS: MSET odd positions} {
        set keys [r debug audit-keys MSET k1 v1 k2 v2 k3 v3]
        assert_equal 3 [llength $keys]
        assert_equal "k1" [lindex $keys 0]
        assert_equal "k2" [lindex $keys 1]
        assert_equal "k3" [lindex $keys 2]
    }

    test {DEBUG AUDIT-KEYS: SMOVE first two args} {
        set keys [r debug audit-keys SMOVE src dst member]
        assert_equal 2 [llength $keys]
        assert_equal "src" [lindex $keys 0]
        assert_equal "dst" [lindex $keys 1]
    }

    test {DEBUG AUDIT-KEYS: BLPOP all but last} {
        set keys [r debug audit-keys BLPOP k1 k2 5]
        assert_equal 2 [llength $keys]
        assert_equal "k1" [lindex $keys 0]
        assert_equal "k2" [lindex $keys 1]
    }

    test {DEBUG AUDIT-KEYS: BRPOPLPUSH first two} {
        set keys [r debug audit-keys BRPOPLPUSH src dst timeout]
        assert_equal 2 [llength $keys]
        assert_equal "src" [lindex $keys 0]
        assert_equal "dst" [lindex $keys 1]
    }

    test {DEBUG AUDIT-KEYS: ZUNIONSTORE destkey + numkeys} {
        set keys [r debug audit-keys ZUNIONSTORE dest 3 k1 k2 k3]
        assert_equal 4 [llength $keys]
        assert_equal "dest" [lindex $keys 0]
        assert_equal "k1" [lindex $keys 1]
        assert_equal "k2" [lindex $keys 2]
        assert_equal "k3" [lindex $keys 3]
    }

    test {DEBUG AUDIT-KEYS: ZUNION numkeys-based} {
        set keys [r debug audit-keys ZUNION 2 k1 k2]
        assert_equal 2 [llength $keys]
        assert_equal "k1" [lindex $keys 0]
        assert_equal "k2" [lindex $keys 1]
    }

    test {DEBUG AUDIT-KEYS: BITOP from 2nd arg} {
        set keys [r debug audit-keys BITOP AND dest k1 k2 k3]
        assert_equal 4 [llength $keys]
        assert_equal "dest" [lindex $keys 0]
        assert_equal "k1" [lindex $keys 1]
        assert_equal "k2" [lindex $keys 2]
        assert_equal "k3" [lindex $keys 3]
    }

    test {DEBUG AUDIT-KEYS: SORT first arg} {
        set keys [r debug audit-keys SORT mykey LIMIT 0 10 STORE dest]
        assert_equal 1 [llength $keys]
        assert_equal "mykey" [lindex $keys 0]
    }

    test {DEBUG AUDIT-KEYS: XREAD STREAMS keys} {
        set keys [r debug audit-keys XREAD COUNT 2 STREAMS s1 s2 0 0]
        assert_equal 2 [llength $keys]
        assert_equal "s1" [lindex $keys 0]
        assert_equal "s2" [lindex $keys 1]
    }

    test {DEBUG AUDIT-KEYS: XREADGROUP STREAMS keys} {
        set keys [r debug audit-keys XREADGROUP GROUP g c STREAMS s1 s2 s3 0 0 0]
        assert_equal 3 [llength $keys]
        assert_equal "s1" [lindex $keys 0]
        assert_equal "s2" [lindex $keys 1]
        assert_equal "s3" [lindex $keys 2]
    }

    test {DEBUG AUDIT-KEYS: EXISTS all args are keys} {
        set keys [r debug audit-keys EXISTS k1 k2]
        assert_equal 2 [llength $keys]
        assert_equal "k1" [lindex $keys 0]
        assert_equal "k2" [lindex $keys 1]
    }

    test {DEBUG AUDIT-KEYS: UNLINK all args are keys} {
        set keys [r debug audit-keys UNLINK k1 k2]
        assert_equal 2 [llength $keys]
        assert_equal "k1" [lindex $keys 0]
        assert_equal "k2" [lindex $keys 1]
    }

    test {DEBUG AUDIT-KEYS: TOUCH all args are keys} {
        set keys [r debug audit-keys TOUCH k1 k2]
        assert_equal 2 [llength $keys]
        assert_equal "k1" [lindex $keys 0]
        assert_equal "k2" [lindex $keys 1]
    }

    test {DEBUG AUDIT-KEYS: MGET all args are keys} {
        set keys [r debug audit-keys MGET k1 k2 k3]
        assert_equal 3 [llength $keys]
        assert_equal "k1" [lindex $keys 0]
        assert_equal "k2" [lindex $keys 1]
        assert_equal "k3" [lindex $keys 2]
    }
}

start_server {tags {"audit-log"}} {
    test {DEBUG AUDIT-PARAM: simple command no encryption} {
        assert_equal "SET mykey myvalue" [r debug audit-param 0 SET mykey myvalue]
    }

    test {DEBUG AUDIT-PARAM: simple command with encryption} {
        # "myvalue" = 7 chars: half=3, 3 plain + 4 stars = "myv****"
        assert_equal "SET mykey myv****" [r debug audit-param 1 SET mykey myvalue]
    }

    test {DEBUG AUDIT-PARAM: encryption masks even-length value} {
        # "HelloWorld" = 10 chars: half=5, 5 plain + 5 stars = "Hello*****"
        assert_equal "SET key Hello*****" [r debug audit-param 1 SET key HelloWorld]
    }

    test {DEBUG AUDIT-PARAM: key is not encrypted} {
        # "secret" = 6 chars: half=3, 3 plain + 3 stars = "sec***"
        assert_equal "SET realkey sec***" [r debug audit-param 1 SET realkey secret]
    }

    test {DEBUG AUDIT-PARAM: MSET keys not encrypted, values encrypted} {
        # "v1" = 2 chars: half=1, 1 plain + 1 star = "v*"
        # "v2" = 2 chars: half=1, 1 plain + 1 star = "v*"
        assert_equal "MSET k1 v* k2 v*" [r debug audit-param 1 MSET k1 v1 k2 v2]
    }

    test {DEBUG AUDIT-PARAM: truncation without encryption} {
        # Build a param that is exactly 5 chars per arg max
        # For SET with 3 args (SET, key, value), max_per_arg = 1024 / 2 = 512
        # Use a 10-char value within limit, no truncation
        assert_equal "SET k abcdefghij" [r debug audit-param 0 SET k abcdefghij]
    }

    test {DEBUG AUDIT-PARAM: truncation with encryption on long value} {
        # Create a long value to trigger truncation
        # For SET k <longval>, max_per_arg = 1024 / 2 = 512
        # Value >= 512 chars triggers truncation, first half plain + half stars + suffix
        set longval [string repeat "x" 600]
        set param [r debug audit-param 1 SET k $longval]
        # Output: SET k + 256 x's + 256 *'s + ...(88 more bytes)
        assert_equal [string length $param] [expr {4 + 1 + 1 + 1 + 512 + 17}]
        # Verify key is in plaintext
        assert_match "SET k x*x*...(*more bytes)" $param
    }

    test {DEBUG AUDIT-PARAM: command name not counted in denominator} {
        # For a command with many args, each arg gets less space
        # MGET k1 k2: argc=3, max_per_arg = 1024/2 = 512
        assert_equal "MGET k1 k2" [r debug audit-param 0 MGET k1 k2]
    }

    test {DEBUG AUDIT-PARAM: DEL all keys not encrypted} {
        assert_equal "DEL k1 k2 k3" [r debug audit-param 0 DEL k1 k2 k3]
    }

    test {DEBUG AUDIT-PARAM: DEL with encryption (keys not masked)} {
        assert_equal "DEL k1 k2 k3" [r debug audit-param 1 DEL k1 k2 k3]
    }

    test {DEBUG AUDIT-PARAM: BLPOP with encryption} {
        assert_equal "BLPOP k1 k2 *" [r debug audit-param 1 BLPOP k1 k2 5]
    }
}
