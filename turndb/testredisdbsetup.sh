#!/bin/sh

redis-cli <<!

SELECT 0
AUTH turn

set turn/user/ninefingers/key "bc807ee29df3c9ffa736523fb2c4e8ee"
set turn/user/gorst/key "7da2270ccfa49786e0115366d3a3d14d"

set turn/user/ninefingers/password "youhavetoberealistic"
set turn/user/gorst/password "hero"

set turn/secret/1368426581 "logen"

set turn/denied-peer-ip/123456 "172.17.13.133-172.17.14.56"
set turn/denied-peer-ip/234567 "123::45"

set turn/allowed-peer-ip/345678 "172.17.13.200"

set turn/origin/http://crinna.org crinna.org
set turn/origin/http://bligh.edu crinna.org

set turn/realm/crinna.org/db 7

save

SELECT 7
AUTH turn

set turn/user/whirrun/key "6972e85e51f36e53b0b61759c5a5219a"
set turn/user/stranger-come-knocking/key "d43cb678560259a1839bff61c19de15e"

set turn/user/whirrun/password "sword"
set turn/user/stranger-come-knocking/password "civilization"

set turn/secret/777888999 "civilization"

save

!
