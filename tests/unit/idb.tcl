start_server {tags {"idb"}} {

    test {SUBKEYS to get all keys} {
        foreach key {key_x key_y key_z} {
            r set $key hello
        }
        r save
        lsort [r subkeys]
    } {key_x key_y key_z}

}
