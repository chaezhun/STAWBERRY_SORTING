# AI-G board pipeline - deploying the strawberry model on the NPU

Written by Kang Yohan, who handled training and NPU deployment, and shared with the
control and integration side of the team. It records the whole path from dataset to
on-board inference, and it is the reference for how detection classes reach the
controller.

---

## 0. The whole flow

```
[train]    PyTorch / Ultralytics YOLOv8n
             build the dataset -> train on Colab -> best.pt
                 |
[convert]  tc-nn-toolkit (WSL, EnlightSDK)
             1) ONNX export      best.pt  -> best.onnx
             2) ONNX extract     best.onnx -> best_extracted.onnx (post-process split off)
             3) default box gen  best.onnx -> best.bin (grid information)
             4) converter        -> best.enlight (FP32)
             5) quantizer        -> best_quantized.enlight (INT8)
             6) compiler         -> net.h, post_process.c, npu_cmd.bin, quantized_network.bin
                 |
[build]    ARM aarch64 cross compile
             net.so
                 |
[deploy]   scp to the board at 192.168.0.100
             /usr/share/strawberry_shifted_v2_quantized/
                 |
[run]      tcnnapp -> camera input -> NPU inference -> detections on stdout
```

The backbone of the work is the vendor course material: the same command sequence,
with the strawberry model substituted for the example one.

---

## 1. Where this came from

The Telechips course walks up through the NPU workflow one step at a time.

| Example | Model | What it teaches | How it was used |
|---|---|---|---|
| Day 6, 02 | LeNet-5, classification | the basic convert / quantize / compile flow | learning the command structure |
| Day 6, 06 | YOLOv8s, detection | converting a detection model and splitting off post-processing | the main reference; our model is the same family |
| Day 7, 02 | UFLD, lane detection | editing the tc-nn-app source, custom post-processing | reference for visualisation and labels |

The strawberry YOLOv8n follows the day 6 example almost exactly. The `yolov8s_convert`
function was used as written, changing only the input filenames and the class count.

```
course YOLOv8s              our model
yolov8s_extracted.onnx  ->  best_extracted.onnx
yolov8s.bin             ->  best.bin
--num-class 80 (COCO)   ->  --num-class N
everything else         ->  unchanged
```

---

## 2. Dataset and training

### Dataset

Model strawberries in three states were photographed from a range of angles and
distances and labelled by hand in LabelImg. The final set is 539 images, 475 for
training and 64 for validation, with extra coverage for the rotten class. The split
is by shooting session rather than by image, so no scene appears on both sides of it.

### Training

```python
from ultralytics import YOLO

model = YOLO("yolov8n.pt")     # nano; the board has 2 GB
model.train(
    data="data.yaml",
    epochs=800,
    imgsz=640,                 # this value has to stay consistent all the way through
    batch=16,
)
```

Output: `best.pt`, mAP50 around 0.99.

The 640 image size carries through ONNX export, the converter and on-board inference.
If any one of those disagrees, the box coordinates come out wrong.

---

## 3. ONNX export

```bash
cd ~/Work/tc-nn-toolkit
source venv/bin/activate     # torch 1.12.0+cpu, ultralytics 8.4.46
```

The first attempt used `opset=12, simplify=True`, and that turned out to matter:

```python
from ultralytics import YOLO
model = YOLO("best.pt")

# first attempt
model.export(format="onnx", imgsz=640, opset=12, simplify=True)
# simplify=True folds the graph and erases the cv2/cv3 node names,
# which the extract step needs to find its outputs

# what actually works, matching the course example
model.export(format="onnx", opset=10)
```

| Option | Value | Why |
|---|---|---|
| `opset` | 10 | 12 and above emit operators the toolkit does not support |
| `simplify` | left off | turning it on removes the node names, making extract impossible |

The export reports:

```
input  (1, 3, 640, 640)
output (1, 6, 8400)
```

which reads as batch 1, then 4 box coordinates plus 2 class scores, then the anchor
count 80x80 + 40x40 + 20x20. The class count is visible in the second dimension, so
this line alone confirms how many classes the model has.

Even with `simplify` off the node *names* get rewritten, but the *weight* names such
as `model.22.cv2.0.2.weight` survive. That is what the next step keys off.

---

## 4. Extracting the raw head outputs

A stock YOLOv8 ONNX has DFL decode, sigmoid and NMS built into the end of the graph.
The NPU is good at convolution, batch norm and activations, and bad at the DFL
softmax, at NMS and at sigmoid. So the network runs on the NPU only as far as the raw
convolution outputs of the head, and the rest happens on the Cortex-A53.

The detect head has six outputs, one classification and one regression tensor per
stride:

```
cv3.0.2  cls, stride 8   [1, nc, 80, 80]
cv2.0.2  reg, stride 8   [1, 64, 80, 80]     64 = reg_max(16) x 4
cv3.1.2  cls, stride 16  [1, nc, 40, 40]
cv2.1.2  reg, stride 16  [1, 64, 40, 40]
cv3.2.2  cls, stride 32  [1, nc, 20, 20]
cv2.2.2  reg, stride 32  [1, 64, 20, 20]
```

Since the node names are gone, they are found through the weights:

```python
import onnx
m = onnx.load('best.onnx')

target_weights = {
    "cls_s8":  "model.22.cv3.0.2.weight",   # shape [nc, 64, 1, 1]
    "reg_s8":  "model.22.cv2.0.2.weight",   # shape [64, 64, 1, 1]
    "cls_s16": "model.22.cv3.1.2.weight",
    "reg_s16": "model.22.cv2.1.2.weight",
    "cls_s32": "model.22.cv3.2.2.weight",
    "reg_s32": "model.22.cv2.2.2.weight",
}
# resolves to onnx::Reshape_314, _328, _342, _357, _371, _385
```

The first dimension of each `cv3.*.weight` should equal the class count. If it does,
training produced what was intended.

```python
import onnx

OUTPUT_LAYER = [
    "onnx::Reshape_357",  # cv3.0.2, cls stride 8
    "onnx::Reshape_314",  # cv2.0.2, reg stride 8
    "onnx::Reshape_371",  # cv3.1.2, cls stride 16
    "onnx::Reshape_328",  # cv2.1.2, reg stride 16
    "onnx::Reshape_385",  # cv3.2.2, cls stride 32
    "onnx::Reshape_342",  # cv2.2.2, reg stride 32
]
onnx.utils.extract_model(
    "best.onnx", "best_extracted.onnx",
    ["images"],
    OUTPUT_LAYER,
)
```

Node count drops from 231 to 207, removing the 24 post-processing nodes. The course
provides `extract_yolo_onnx.sh` for this, which calls the same function.

---

## 5. Default boxes

The grid and anchor information goes into a separate binary so the on-board
post-processing can map a pixel position back to its grid cell.

```bash
python tools/gen_default_box_tools/default_box_generator.py \
    input_networks/strawberry/best.onnx \
    --output input_networks/strawberry/best.bin \
    --version v8 \
    --grid-layer-name onnx::Reshape_314 onnx::Reshape_328 onnx::Reshape_342
```

The three named layers are the regression outputs at strides 8, 16 and 32.

The generated file broke the converter:

```
generated best.bin      GENBIN_V1, 134528 bytes
course yolov8s.bin      FENBIN_V1,  67328 bytes

Exception: [Error] YOLO grid file is invalid.
           Network output and grid shape do not match
```

Within the same toolkit, `default_box_generator` writes the newer format while
`converter.py` still expects the older one. The fix was to reuse the course file:

```bash
cp input_networks/Education/ai_model/yolov8s.bin \
   input_networks/strawberry/best.bin
```

That is legitimate because the grid does not depend on backbone size. The s, m and n
variants all use strides 8/16/32, `reg_max` 16 and a 640x640 input, so the grid
mapping is identical, and the class count comes from the converter's `--num-class`
rather than from this file. With a later toolkit version the final model generated a
correct 67328-byte `best_shifted_v2.bin`.

---

## 6. Converter, to FP32 enlight

Quantisation needs activation statistics, which means calibration images. They have
to be the project's own training images for the accuracy to hold.

```bash
cp ~/Work/strawberry_dataset/train/images/*.jpg my_dataset_path/
```

```bash
python ./EnlightSDK/converter.py \
    input_networks/strawberry/best_extracted.onnx \
    --type obj \
    --add-detection-post-process input_networks/strawberry/best.bin \
    --dataset Custom \
    --dataset-root my_dataset_path \
    --output output_networks/best.enlight \
    --enable-track \
    --mean 0 0 0 \
    --std 1 1 1 \
    --num-class 4 \
    --yolo-version v8 \
    --enable-letterbox \
    --dfl-reg-max 16 \
    --output-order cl \
    --logistic sigmoid \
    --no-background
```

| Option | Meaning | Why |
|---|---|---|
| `--type obj` | object detection | YOLO is a detection task |
| `--add-detection-post-process` | inject the grid | needed by the on-board post-processing |
| `--dataset Custom --dataset-root` | calibration images | source of the quantisation statistics |
| `--enable-track` | collect activation statistics | the basis of INT8 quantisation |
| `--mean 0 0 0 --std 1 1 1` | no normalisation | YOLOv8 only scales to 0-1; it does not use ImageNet statistics |
| `--num-class 4` | class count | four after the label shift described below |
| `--yolo-version v8` | v8 detect head | selects how the head is handled |
| `--enable-letterbox` | aspect-preserving resize | objects are not squashed |
| `--dfl-reg-max 16` | DFL bin count | must match training |
| `--output-order cl` | classification first | the memory order the NPU expects |

Result:

```
Number of compatible layers   : 216
Number of incompatible layers :   0
Total number of layers        : 216

output_networks/best.enlight, about 12 MB
```

Zero incompatible layers means the whole network maps onto the NPU, so nothing like
the unsupported reshape in the lane-detection example applies here.

---

## 7. The class 0 problem and the label shift

This is the most important finding in the project, and it is what determines the
class mapping the controller sees.

On the PC simulator every class is detected. On the board, class index 0 never
appears:

```
enlight_sim (INT8)          board (tcnnapp)
  class 0  ok, 93%            class 0  never appears
  class 1  ok, 95%            class 1  ok
  class 2  ok, 94%            class 2  ok
```

It was isolated as follows. Changing converter options improved other things but not
this. Running the same quantised model through `enlight_sim` on the PC detected class
0 correctly, so the file is fine. The MD5 of `net.so` matched on both sides, so the
transfer is fine. Running the board with `-g log` to print raw class indices showed
class 0 never being emitted at all.

The conclusion is that the tcnnapp post-processing treats class index 0 as
background and drops it.

The workaround is to shift every training label up by one and leave class 0 empty:

```bash
for f in train/labels/*.txt valid/labels/*.txt; do
    awk '{$1=$1+1; print}' "$f" > "$f.tmp" && mv "$f.tmp" "$f"
done
```

```
before, nc = 3               after, nc = 4
  0 = fresh   (never seen)     0 = dummy, unused
  1 = unripe                   1 = fresh
  2 = rotten                   2 = unripe
                               3 = rotten
```

Every real class now sits at index 1 or above. The model was retrained and converted
with `--num-class 4`.

### What the controller receives

tcnnapp shifts the index by one again on output, so the mapping from the wire format
to the project classes is:

```
tcnnapp     meaning     project class     action
 class 2     fresh       0 fresh          plate stays level, fruit passes
 class 3     unripe      2 unripe         plate tilts right
 class 4     rotten      1 rotten         plate tilts left
 anything else           dropped          not transmitted
```

This is why `ai_result_sender.sh` parses only classes 2, 3 and 4 and skips the rest.

---

## 8. Quantizer, FP32 to INT8

```bash
python ./EnlightSDK/quantizer.py \
    output_networks/best.enlight \
    --output output_networks/best_quantized.enlight \
    --m-std-4 7
```

It loads the FP32 weights together with the activation statistics the converter
gathered, computes a per-layer scale and zero point, converts the weights to INT8 at
roughly a quarter of the size, and writes the result:

```
quantized_value = round(real_value / scale) + zero_point
```

`--m-std-4` sets the activation clipping range. The default of 5 lost too much of the
difference between classes once compressed to INT8, so it was raised to 7.

Verify with `enlight_sim.py` on the PC before flashing the board. One to three
percent of accuracy lost to quantisation is normal; ten percent or more points at
something else, usually too little calibration data.

---

## 9. Compiler

```bash
python ./EnlightSDK/compiler.py \
    output_networks/best_quantized.enlight \
    --th-iou 0.5 \
    --th-conf 0.5
```

Outputs:

```
net.h                    network structure header
post_process.c           model-specific post-processing (DFL decode, sigmoid, NMS)
npu_cmd.bin              NPU instruction sequence
quantized_network.bin    INT8 weights
```

`--th-conf` discards detections below 0.5 confidence and `--th-iou` sets the NMS
overlap threshold. The compiler notification should show cv3 mapped to the confidence
layers and cv2 to the location layers.

---

## 10. Building net.so

```bash
cd build_network/
# symlink network.h and post_process.c, then
make
```

Toolchain: `gcc-arm-9.2-2019.12-x86_64-aarch64-none-linux-gnu`.

tcnnapp itself stays as it is; swapping `net.so` is enough to change models. The
lane-detection example needed a full bitbake rebuild because of its custom
post-processing, but a standard YOLOv8 head does not.

---

## 11. Deployment

```bash
scp net.so npu_cmd.bin quantized_network.bin \
    root@192.168.0.100:/usr/share/strawberry_shifted_v2_quantized/

md5sum net.so npu_cmd.bin quantized_network.bin
```

The three files in `/usr/share/strawberry_shifted_v2_quantized/` are one set: the
post-processing logic, the NPU instructions and the INT8 weights.

---

## 12. Running inference

```bash
tcnnapp \
    -n /usr/share/strawberry_shifted_v2_quantized/ \
    -i camera \
    -p /dev/video2 \
    -o display \
    -w 1280 -h 720 \
    -W 800 -H 480 \
    -g log
```

| Option | Meaning |
|---|---|
| `-n` | model directory, where the compiler output lives |
| `-i camera` | input mode |
| `-p /dev/video2` | V4L2 device |
| `-o display` | output over HDMI |
| `-w -h` | input resolution |
| `-W -H` | output resolution |
| `-g log` | debug log, which is where raw class indices appear |

In normal operation `ai_result_sender.sh` parses this output with awk and sends it to
the D3-G board over TCP. A detection line looks like:

```
x1 y1 x2 y2 = 321.5 277.5 509.9 642.3 class: 3 score: 82.89
```

The script takes the class and the box centre and sends `CLASS,CONF,X,Y`.

---

## 13. Things that went wrong

| Symptom | Cause and fix |
|---|---|
| extract cannot find the cv2/cv3 nodes | `opset=12` with `simplify=True` erased the names; use opset 10 with simplify off |
| converter reports "YOLO grid file is invalid" | the generated `.bin` is a newer format than the converter accepts; reuse the course `yolov8s.bin` |
| class 0 never detected on the board | tcnnapp treats index 0 as background; shift all labels up by one and leave class 0 empty |
| classes poorly separated after quantisation | raise `--m-std-4` from 5 to 7 |
| board misclassifies while the PC is correct | `--logistic` and `--no-background` were unset; specify them explicitly |

---

## 14. The full sequence

```bash
# ONNX export
python3 -c "from ultralytics import YOLO; YOLO('best.pt').export(format='onnx', opset=10)"

# extract the six head outputs
python3 extract_strawberry.py

# default boxes, or reuse the course file
cp input_networks/Education/ai_model/yolov8s.bin input_networks/strawberry/best.bin

# converter
python ./EnlightSDK/converter.py \
    input_networks/strawberry/best_extracted.onnx \
    --type obj --add-detection-post-process input_networks/strawberry/best.bin \
    --dataset Custom --dataset-root my_dataset_path \
    --output output_networks/best.enlight \
    --enable-track --mean 0 0 0 --std 1 1 1 \
    --num-class 4 --yolo-version v8 --enable-letterbox \
    --dfl-reg-max 16 --output-order cl --logistic sigmoid --no-background

# quantizer
python ./EnlightSDK/quantizer.py \
    output_networks/best.enlight \
    --output output_networks/best_quantized.enlight --m-std-4 7

# compiler
python ./EnlightSDK/compiler.py \
    output_networks/best_quantized.enlight --th-iou 0.5 --th-conf 0.5

# build the post-processing library
cd build_network && make

# deploy
scp net.so npu_cmd.bin quantized_network.bin \
    root@192.168.0.100:/usr/share/strawberry_shifted_v2_quantized/

# run
tcnnapp -n /usr/share/strawberry_shifted_v2_quantized/ \
    -i camera -p /dev/video2 -o display -w 1280 -h 720 -W 800 -H 480 -g log
```

---

## 15. What the control side needs to know

1. The class indices are shifted by one because of the NPU behaviour above. tcnnapp
   emits class 2 for fresh, 3 for unripe and 4 for rotten, and `ai_result_sender.sh`
   maps those onto the project classes 0, 1 and 2 before transmitting. The controller
   uses the received value as it stands.
2. The model lives in `/usr/share/strawberry_shifted_v2_quantized/` as a set of three
   files. Changing models means replacing all three.
3. Retraining and reconverting follows the sequence in section 14 unchanged apart
   from `--num-class`, but the labels must stay shifted so that class 0 is empty.
