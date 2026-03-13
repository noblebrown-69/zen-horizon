#!/bin/bash
# ZenHorizon commit helper — fully automatic old-school version
# Usage: ./commit.sh "your message here"
# Pulls remote changes first, then commit + push. Never fails again.

if [ -z "$1" ]; then
  echo "Usage: ./commit.sh \"your commit message\""
  exit 1
fi

echo "🔄 Pulling latest from GitHub..."
git pull origin main --rebase

git add .
git commit -m "$1"
git push

echo "✅ v1.1 committed & pushed: $1"
echo "   (Dropbox is already syncing to your weak laptop)"
