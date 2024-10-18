#!/bin/bash

__package_manager_update () {
    log "updating package manager apt..."
    sudo apt-get update
}


__package_manager_install () {
    # $1: package name from package manager
    sudo apt-get install -y --allow-downgrades $1
}


util_check_dep_retval=0
util_check_dep () {
    # $1 cmd line binary name
    # $2: package name from package manager
    if [[ ! -x "$(command -v $1)" ]]; then
        warn "check dependencies [$2]: failed"
        util_check_dep_retval=0
    else
        log "check dependencies [$2]: success"
        util_check_dep_retval=1
    fi
}


util_install_common () {
    # $1: command line name, "*" for no command line checking
    # $2: package name from package manager
    if [ "$1" = "*" ]; then
        warn "check dependencies [$2]: skipped checking, directly install"
        __package_manager_install $2
    else
        util_check_dep $1 $2
        if [[ $util_check_dep_retval -eq 0 ]]; then
            __package_manager_install $2
            # check again
            util_check_dep $1 $2
            if [[ $util_check_dep_retval -eq 0 ]]; then
                error "failed to install $2"
            fi
        fi
    fi   
}


check_and_install_go() {
    if [[ ! -x "$(command -v go)" ]]; then
        warn "no go installed, installing from assets..."
        cd $DIR_ASSETS
        rm -rf /usr/local/go
        tar -C /usr/local -xzf go1.23.2.linux-amd64.tar.gz
        echo 'export PATH=$PATH:/usr/local/go/bin' >> $HOME/.bashrc
        source $HOME/.bashrc
        if [[ ! -x "$(command -v go)" ]]; then
            error "failed to install golang runtime"
        fi
        log "go1.23.2 installed"
        warn "please \"source $HOME/.bashrc\" for loading golang"
    fi
}

# __package_manager_update
