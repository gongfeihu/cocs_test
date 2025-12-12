# modify_large_file.py
import random
import sys
import os

def generate_random_chars(n):
    import string
    chars = string.ascii_letters + string.digits + '!@#$%^&*()_+-=[]{}|;:,.<>?~` '
    return ''.join(random.choices(chars, k=n))

def main(input_path, output_path):
    file_size = os.path.getsize(input_path)
    if file_size == 0:
        print("文件为空")
        return

    # 读取整个文件（50MB 在现代机器上可行）
    with open(input_path, 'rb') as f:
        data = bytearray(f.read())  # 用 bytearray 支持修改

    print(f"原文件大小: {len(data)} 字节")

    # === 步骤1: 随机删除 10 处，每处 100 字符 ===
    delete_ops = []
    for _ in range(10):
        pos = random.randint(0, len(data) - 100)  # 确保不越界
        delete_ops.append(pos)
    
    # 从后往前删，避免位置偏移
    delete_ops.sort(reverse=True)
    for pos in delete_ops:
        del data[pos:pos+100]
        print(f"在位置 {pos} 删除 100 字符")

    # === 步骤2: 随机插入 20 处，每处 200 字符 ===
    insert_ops = []
    for _ in range(20):
        pos = random.randint(0, len(data))  # 可插入到末尾
        insert_ops.append((pos, generate_random_chars(200)))
    
    # 从后往前插，避免位置偏移
    insert_ops.sort(key=lambda x: x[0], reverse=True)
    for pos, text in insert_ops:
        data[pos:pos] = text.encode('latin1')  # 保持字节兼容
        print(f"在位置 {pos} 插入 200 字符")

    # 写入新文件
    with open(output_path, 'wb') as f:
        f.write(data)

    print(f"修改完成！新文件大小: {len(data)} 字节")
    print(f"输出文件: {output_path}")

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print("用法: python3 modify_large_file.py <输入文件> <输出文件>")
        sys.exit(1)
    main(sys.argv[1], sys.argv[2])