#!/bin/bash
# ZenHorizon commit helper — fully automatic, SSH, zero passwords forever
# Usage: ./commit.sh "your message here"

if [ -z "$1" ]; then
  echo "Usage: ./commit.sh \"your commit message\""
  exit 1
fi

echo "🔄 Pulling latest..."
git pull origin main --rebase

git add .
git commit -m "$1" || echo "✓ Nothing new to commit"
git push

echo "✅ Committed & pushed: $1"
echo "   (Dropbox syncing to weak laptop automatically)"
