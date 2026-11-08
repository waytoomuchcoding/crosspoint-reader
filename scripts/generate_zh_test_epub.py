#!/usr/bin/env python3
"""Generate a small Simplified Chinese EPUB for testing the Chinese reading mode.

The text is original sample prose written for testing. It mixes common HSK 1-3
words with rarer characters (林, 厚, 轮, 料, 填, 梦, 蜻蜓, 琥珀...) so ruby and
lookup have something to annotate.

Usage: python3 scripts/generate_zh_test_epub.py [output.epub]
"""

import sys
import zipfile
from html import escape

TITLE = "林夏的一天"
AUTHOR = "测试文本"

CHAPTERS = [
    ("第一章 银行", [
        "周六早上，林夏要去银行办一张新卡。银行门口的行人很多，大家都在排队。她看着手机，觉得时间过得很慢。前面的老人说：“这条队太长了，我还是明天再来吧。”林夏笑了笑，说：“没关系，我不着急。”",
        "她从小在这个城市长大，可是对首都的生活还不太了解。她的爱好是听音乐和看书，每个周末都会去图书馆还书，再借几本新的。今天的书包很重，因为里面有三本厚厚的小说。",
        "轮到她的时候，工作人员看了看她的资料，说：“不行，这张表要重新填。”林夏心想：行，那就再写一遍。她只好拿着笔，一个字一个字地写。写完以后，她得到了一张新卡，心里很快乐。",
        "回家的路上，她沿着河边慢慢地行走。风吹过来，带着一点花香。她想：生活就是这样，有时候要排很长的队，有时候要重新开始，可是只要心里有喜欢的东西，就不会觉得累。",
        "晚上，她早早地睡觉了。梦里，她又回到了银行门口，那位老人对她说：“你看，今天人少多了，我们一起进去吧。”她点点头，觉得这个梦很温暖。",
    ]),
    ("第二章 河边", [
        "星期天下午，天气很好。林夏带着一本旧书和一瓶水，又来到了河边。河水很清，几只蜻蜓在水面上飞来飞去，偶尔停在芦苇的叶子上。",
        "一个小男孩蹲在石头旁边，手里拿着一块黄色的石头。他问林夏：“姐姐，这是不是琥珀？”林夏接过来看了看，石头里面好像真的有一只很小的虫子。她说：“我也不知道，不过它真漂亮。你可以带回家，问问你的老师。”",
        "男孩高兴地跑走了。林夏坐在长椅上，翻开书。书里讲的是一个渔夫的故事：他每天清晨划着小船出发，傍晚才回来，船上总是装满了鱼。有一天，他在网里发现了一个生锈的铁盒子。",
        "她读得很入迷，连太阳快要落山都没有注意到。直到一阵凉风吹过，她才合上书，慢慢地走回家。路边的灯一盏一盏地亮了起来。",
    ]),
]

CONTAINER = """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
"""


def chapter_xhtml(title, paragraphs):
    body = "\n".join(f"    <p>{escape(p)}</p>" for p in paragraphs)
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="zh-CN" lang="zh-CN">
  <head><title>{escape(title)}</title></head>
  <body>
    <h2>{escape(title)}</h2>
{body}
  </body>
</html>
"""


def content_opf():
    manifest = "\n".join(
        f'    <item id="ch{i}" href="ch{i}.xhtml" media-type="application/xhtml+xml"/>'
        for i in range(len(CHAPTERS)))
    spine = "\n".join(f'    <itemref idref="ch{i}"/>' for i in range(len(CHAPTERS)))
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="uid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="uid">crosspoint-zh-test-0001</dc:identifier>
    <dc:title>{TITLE}</dc:title>
    <dc:creator>{AUTHOR}</dc:creator>
    <dc:language>zh-CN</dc:language>
    <meta property="dcterms:modified">2026-11-07T00:00:00Z</meta>
  </metadata>
  <manifest>
    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
{manifest}
  </manifest>
  <spine>
{spine}
  </spine>
</package>
"""


def nav_xhtml():
    items = "\n".join(
        f'      <li><a href="ch{i}.xhtml">{escape(t)}</a></li>' for i, (t, _) in enumerate(CHAPTERS))
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
  <head><title>{TITLE}</title></head>
  <body>
    <nav epub:type="toc"><ol>
{items}
    </ol></nav>
  </body>
</html>
"""


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "zh-test.epub"
    with zipfile.ZipFile(out, "w") as z:
        z.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml", CONTAINER, compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/content.opf", content_opf(), compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/nav.xhtml", nav_xhtml(), compress_type=zipfile.ZIP_DEFLATED)
        for i, (title, paras) in enumerate(CHAPTERS):
            z.writestr(f"OEBPS/ch{i}.xhtml", chapter_xhtml(title, paras), compress_type=zipfile.ZIP_DEFLATED)
    print(out)


if __name__ == "__main__":
    main()
