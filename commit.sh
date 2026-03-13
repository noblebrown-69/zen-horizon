#!/bin/bash
# ZenHorizon commit helper — old-school single command for life
# Usage: ./commit.sh "your message here"
# Keeps your repo clean, Dropbox synced, and GitHub happy forever

if [ -z "$1" ]; then
  echo "Usage: ./commit.sh \"your commit message\""
  exit 1
fi

git add .
git commit -m "$1"
git push

echo "✅ Committed & pushed: $1"
echo "   (Dropbox is already syncing this to your weak laptop)"
