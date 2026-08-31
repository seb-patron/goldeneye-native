# Wiki source

These pages are the GitHub wiki, kept in the repository so they version with the code and get
reviewed in the same pull request.

To publish, copy them into the wiki repo:

```
git clone https://github.com/seb-patron/goldeneye-native.wiki.git
cp wiki/*.md goldeneye-native.wiki/
cd goldeneye-native.wiki && git add -A && git commit -m "wiki: sync from main" && git push
```

`Home.md` is the landing page. GitHub turns a filename into a page title, so `Frame-timing.md`
becomes the page linked as `[[Frame timing]]` or `Frame-timing`.
