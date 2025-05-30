#!/bin/bash

# usage: addenv env_name path
function addenv() {
  sed -i -e "/^export $1=.*/d" ~/.bashrc
  echo -e "\nexport $1=`readlink -e $2`" >> ~/.bashrc
  echo "By default this script will add environment variables into ~/.bashrc."
  echo "After that, please run 'source ~/.bashrc' to let these variables take effect."
  echo "If you use shell other than bash, please add these environment variables manually."
}

# usage: init repo branch directory trace [env]
# trace = true|false
function init() {
  if [ -d $3 ]; then
    echo "$3 is already initialized, skipping..."
    return
  fi

  while [ ! -d $3 ]; do
    git clone -b $2 git@github.com:$1.git $3
  done
  log="$1 `cd $3 && git log --oneline --no-abbrev-commit -n1`"$'\n'

  sed -i -e "/^\/$3/d" .gitignore
  echo "/$3" >> .gitignore
  git add -A .gitignore

  if [ $5 ] ; then
    addenv $5 $3
  fi
}

case $1 in
  nemu)
    init NJU-ProjectN/nemu ics2024 nemu false NEMU_HOME
    ;;
  abstract-machine)
    init NJU-ProjectN/abstract-machine ics2024 abstract-machine false AM_HOME
    init NJU-ProjectN/fceux-am ics2021 fceux-am false
    ;;
  am-kernels)
    init NJU-ProjectN/am-kernels ics2021 am-kernels false
    ;;
  nanos-lite)
    init NJU-ProjectN/nanos-lite ics2021 nanos-lite false
    ;;
  navy-apps)
    init NJU-ProjectN/navy-apps ics2024 navy-apps false NAVY_HOME
    ;;
  *)
    echo "Invalid input..."
    exit
    ;;
esac