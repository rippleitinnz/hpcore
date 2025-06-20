#!/bin/bash
# Vultr API script

# Usage examples:
# ./vultr.sh create mycluster 3
# ./vultr.sh info mycluster
# ./vultr.sh delete mycluster
# ./vultr.sh expand mycluster 2
# ./vultr.sh shrink mycluster 2
# ./vultr.sh regions

planid="vc2-1c-1gb" # $5/month
osid=387 # Ubuntu 20.04
# Order of Vultr regions to distribute servers across the globe.
regions=("syd" "yto" "ams" "atl" "cdg" "dfw" "ewr" "fra" "icn" "lax" "lhr" "mia" "nrt" "ord" "sea" "sgp" "sjc" "sto" "mex")

# jq command is used for json manipulation.
if ! command -v jq &> /dev/null
then
    sudo apt-get install -y jq
fi
if ! command -v curl &> /dev/null
then
    sudo apt-get install -y curl
fi

configfile=config.json
[ ! -f $configfile ] && >&2 echo "config.json not found." && exit 1

apikey=$(jq -r ".vultr.api_key" $configfile)
[ -z $apikey ] && >&2 echo "Vultr api key not found." && exit 1

startscriptid=$(jq -c -r ".vultr.startup_script_id" $configfile)
sshkeyids=$(jq -c -r ".vultr.ssh_key_ids" $configfile)

# Common api calling function. (params: httpmethod, endpoint, bodyparams)
function apicall() {
    local url="https://api.vultr.com/v2/$2"
    local _result=""
    if [ -z "$3" ]; then
        _result=$(curl --silent "$url" -X $1 -H "Authorization: Bearer $apikey" -H "Content-Type: application/json" -w "\n%{http_code}")
    else
        _result=$(curl --silent "$url" -X $1 -H "Authorization: Bearer $apikey" -H "Content-Type: application/json" -w "\n%{http_code}" --data "$3")
    fi
    
    local _parts
    readarray -t _parts < <(printf '%s' "$_result") # break parts by new line.
    if [[ ${_parts[1]} == 2* ]]; then # Check for 2xx status code.
        [ ! -z "${_parts[0]}" ] && echo ${_parts[0]} # Return api output if there is any.
    else
        >&2 echo "Error on $1 $url code:${_parts[1]} body:${_parts[0]}"
        exit 1
    fi
}
function apiget() {
    if [ -z "$2" ]; then
        apicall GET $1
    else
        apicall GET "$1/$2"
    fi
}
function apigetquery() {
    apicall GET $1?$2
}
function apipost() {
    apicall POST $1 "$2"
}
function apidelete() {
    apicall DELETE "$1/$2"
}

# Vultr specific api calls.
function getplans() {
    apiget "plans"
}
function getregions() {
    apiget "regions"
}
function getoses() {
    apiget "os"
}
function getsshkeys() {
    apiget "ssh-keys"
}
function getstartscripts() {
    apiget "startup-scripts"
}

# Generates vm name using the standard pattern. (parmas: groupname, nodenumber)
function vmname() {
    echo $1$(printf "%03d" $2) # Pad node number with 3 zeros.
}

# Creates a vm. (params: groupname, vmname, regionid)
function createvm() {
    echo "Creating vm '$2' in $3..."
    local _vminfo=$(apipost "instances" '{"tag":"'$1'", "label":"'$2'", "region":"'$3'", "os_id":'$osid', "plan":"'$planid'", "hostname":"'$2'", "script_id":"'$startscriptid'", "sshkey_id":'$sshkeyids', "backups":"disabled"}')
    [ -z "$_vminfo" ] && exit 1
    local _vmid=$(echo $_vminfo | jq -r ".instance.id")
    local _vmip=$(echo $_vminfo | jq -r ".instance.main_ip")
    for (( i=0; i<20; i++ ))
    do
        if [ "$_vmip" == "0.0.0.0" ]; then
            sleep 1
            _vminfo=$(apiget "instances" $_vmid)
            _vmip=$(echo $_vminfo | jq -r ".instance.main_ip")
        else
            break
        fi
    done
    echo $2": "$_vmip
}

# Deletes a vm. (params: id)
function deletevm() {
    echo "Deleting vm "$1"..."
    apidelete "instances" $1
}

# Returns space-delimited list of vm field values sorted by vm label in specified group. (params: groupname, fieldname)
function getgroupvmfield() {
    [ -z "$1" ] && >&2 echo "getgroupvmfield: Group name not specified." && exit 1
    [ -z "$2" ] && >&2 echo "getgroupvmfield: Field name not specified." && exit 1
    local _list=$(apigetquery "instances" "tag=$1")
    [ -z "$_list" ] && exit 1
    # Get field values sorted by the vm label.
    local _vals=$(echo $_list | jq -r ".instances | sort_by(.label) | .[] | .$2")
    echo $_vals
}

# Deletes a group of vms. (params: groupname)
function deletevmgroup() {
    echo "Deleting all vms in group '"$1"'..."
    local _ids=$(getgroupvmfield "$1" "id")
    [ -z "$_ids" ] && exit 1
    local _arr
    readarray -d " " -t _arr < <(printf '%s' "$_ids") # break parts by space character.
    echo ${#_arr[@]}" vms found to delete..."
    for id in "${_arr[@]}"
    do
        deletevm $id &
    done
    wait
    echo "Done."
}

# Creates a group of vms. (params: groupname, count, startnumber)
function createvmgroup() {
    echo "Creating "$2" vms in group '"$1"'..."
    local -i start=$3
    [ $start == 0 ] && start=1
    local -i end=$start+$2
    local -i rcount=${#regions[@]} # region count.
    for (( i=$start; i<$end; i++ ))
    do
        local -i r=$((($i - 1) % $rcount))
        createvm "$1" $(vmname $1 $i) "${regions[$r]}" &
    done
    wait
    echo "Done."
}

# Grows a vm group. (params: groupname, expandbycount)
function expandvmgroup() {
    # Get current no. of vms in the group. (this returns ids sorted by vm label)
    local _ids=$(getgroupvmfield "$1" "id")
    [ -z "$_ids" ] && exit 1
    local _arr
    readarray -d " " -t _arr < <(printf '%s' "$_ids") # break parts by space character.
    local -i _oldcount=${#_arr[@]}
    local -i _newcount=$_oldcount+$2
    [ $_oldcount -ge $_newcount ] && exit 1
    echo "Expanding '"$1"' group from "$_oldcount" to "$_newcount" nodes..."
    let -i startnum=$_oldcount+1
    createvmgroup $1 $2 $startnum
}

# Shrinks a vm group. (params: groupname, shrinkbycount)
function shrinkvmgroup() {
    # Get current set of vms in the group. (this returns ids sorted by vm label)
    local _ids=$(getgroupvmfield "$1" "id")
    [ -z "$_ids" ] && exit 1
    local _arr
    readarray -d " " -t _arr < <(printf '%s' "$_ids") # break parts by space character.
    local -i _oldcount=${#_arr[@]}
    local -i _newcount=$_oldcount-$2
    echo "Shrinking '"$1"' group from "$_oldcount" to "$_newcount" nodes..."
    for (( i=$_newcount; i<$_oldcount; i++ ))
    do
        deletevm ${_arr[$i]} &
    done
    wait
    echo "Done."
}

# DIsplays information about existing cluster. (params: groupname)
function displayinfo() {
    local _ips=$(getgroupvmfield "$1" "main_ip")
    local _arr
    readarray -d " " -t _arr < <(printf '%s' "$_ips") # break parts by space character.
    echo "Total "${#_arr[@]}" vms found in '"$1"'"
    for (( i=0; i<${#_arr[@]}; i++ ))
    do
        let n=$i+1
        echo $n. ${_arr[$i]}
    done
}

mode=$1
name=$2
num=$3

if [ "$mode" = "create" ] || [ "$mode" = "info" ] || [ "$mode" = "delete" ] ||
   [ "$mode" = "expand" ] || [ "$mode" = "shrink" ] || [ "$mode" = "regions" ]; then
    echo "mode: $mode"
else
    echo "Invalid command."
    echo " Expected: create <name> <count> | info <name> | delete <name> | expand <name> <by> | shrink <name> <by> | regions"
    exit 1
fi

if [ $mode = "create" ]; then
    createvmgroup $name $num
    exit 0
fi

if [ $mode = "info" ]; then
    displayinfo $name
    exit 0
fi

if [ $mode = "delete" ]; then
    deletevmgroup $name
    exit 0
fi

if [ $mode = "expand" ]; then
    expandvmgroup $name $num
    exit 0
fi

if [ $mode = "shrink" ]; then
    shrinkvmgroup $name $num
    exit 0
fi

if [ $mode = "regions" ]; then
    getregions
    exit 0
fi