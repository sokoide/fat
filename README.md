# FAT12 lib

## How to make a demo FAT12 imange

* install mtools
  * macOS: `brew install mtools`
  * Linux:: `sudo apt install mtools`
* create a 720KB demo FAT12 image

```sh
export VOL=DEMOF12
export IMG=demof12.fat

rm $IMG
mformat -f 720 -v $VOL -C -i $IMG ::

echo "hello world" > hello.txt
mcopy -i $IMG hello.txt main.c ::

mmd -i $IMG dir1 dir2 dir2/subdir1
mdir -i $IMG
mdir -i $IMG ::dir

echo "hoge" > hoge.txt
echo "page" > page.txt
mcopy -i $IMG hoge.txt ::dir1
mcopy -i $IMG page.txt ::dir2/subdir1

mdir -i $IMG
mdir -i $IMG ::dir1
mdir -i $IMG ::dir2/subdir1

rm hello.txt hoge.txt page.txt
```
