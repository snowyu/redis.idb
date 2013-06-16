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
        lsort [r subkeys "" "" 3 1*3]
    } {key_x key_y key_z}
}
