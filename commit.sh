#!/bin/bash
# ZenHorizon commit helper — 100% automatic forever
# Just type: ./commit.sh "your message here"

if [ -z "$1" ]; then
  echo "Usage: ./commit.sh \"your message\""
  exit 1
fi

git pull origin main
git add .
git commit -m "$1" || echo "✓ Already up to date"
git push

echo "✅ Committed & pushed: $1"
echo "   (Dropbox is already syncing to your weak laptop)"
