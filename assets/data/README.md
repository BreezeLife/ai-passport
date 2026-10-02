English · [简体中文](README.zh_CN.md)

# Dinosaur content

dinosaurs.json is the editable catalog and narration source. Forty dinosaurs each have two short facts, a two-choice observation question, an answer index and a gentle hint. Questions never lock content or deduct points.

Facts were checked against the Natural History Museum pages in each source field. Velociraptor feather evidence was also checked against the [American Museum of Natural History](https://www.amnh.org/explore/news-blogs/velociraptor-feather-evidence). Spinosaurus swimming behavior and the purpose of Stegosaurus plates remain qualified. Art is a stylized teaching aid. The text is an independent child-friendly paraphrase.

After editing, run `python3 tools/generate_dino_catalog.py`, regenerate fonts and narration, then repeat the complete gate.
