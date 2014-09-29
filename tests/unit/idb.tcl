start_server {tags {"idb"}} {

    test {SUBKEYS to get all keys} {
        foreach key {key_x key_y key_z foo_a foo_b foo_c} {
            r set $key hello
        }
        r save
        lsort [r subkeys]
    } {foo_a foo_b foo_c key_x key_y key_z}

    test {SUBKEYS to match foo* keys} {
        lsort [r subkeys "." "foo*"]
    } {foo_a foo_b foo_c}
    test {SUBKEYS to get first page} {
        lsort [r subkeys "" "" 3]
    } {foo_a foo_b foo_c}
    test {SUBKEYS to get second page} {
        lsort [r subkeys "" "" 3 3]
    } {key_x key_y key_z}
    test {SUBKEYS add some keys in memory not flush to disk} {
        foreach key {key_a key_b} {
            r set $key hello
        }
         lsort [r subkeys]
    } {foo_a foo_b foo_c key_a key_b key_x key_y key_z}
    test {SUBKEYS del some keys in memory not flush to disk} {
        foreach key {key_z key_x} {
            r del $key
        }
         lsort [r subkeys]
    } {foo_a foo_b foo_c key_a key_b key_y}


    test {ASET and AGET an item} {
        r aset x ".text" "hello"
        r aget x ".text"
    } "hello"

    test {ASET and AGET an empty item} {
        r aset x ".text" {}
        r aget x ".text"
    } {}

    test {ADEL against a single item} {
        r adel x ".text"
        r aget x ".text"
    } {}

    test {AEXISTS} {
        set res {}
        r aset newkey ".text" test
        append res [r aexists newkey ".text"]
        r adel newkey ".text"
        append res [r aexists newkey ".text"]
    } {10}

    test {Zero length value in key. ASET/AGET/AEXISTS} {
        r aset emptykey ".text" {}
        set res [r aget emptykey ".text"]
        append res [r aexists emptykey ".text"]
        r adel emptykey ".text"
        append res [r aexists emptykey ".text"]
    } {10}

}
