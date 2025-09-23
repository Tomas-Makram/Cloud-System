# Cloud-System
# 🗂️ MiniHSFS – File System Simulation

مشروع تعليمي بلغة **C++** لمحاكاة نظام ملفات مبسط (File System).  
يدعم إنشاء/حذف الملفات والمجلدات، تخزين البيانات باستخدام **Inode Table**،  
وإدارة الكتل الحرة عبر **B-Tree**.  
يتم تخزين البيانات على قرص افتراضي (Virtual Disk) مع دعم التحقق من السلامة (Checksum).  

## ▶️ التشغيل
```bash
g++ -std=c++17 -o MiniHSFS main.cpp
./MiniHSFS


---

## 📌 النسخة التفصيلية (Pro)
```markdown
# 🗂️ MiniHSFS – File System Simulation  

## 📌 الوصف
مشروع **MiniHSFS** هو نظام ملفات (File System) مبسط تم تطويره بلغة **C++** بهدف محاكاة طريقة عمل أنظمة الملفات الحقيقية مثل **NTFS** و **EXT4**.  
يوفر المشروع بنية مرنة لإدارة الملفات والمجلدات باستخدام **Inode Table**، **B-Tree**، و **Virtual Disk** لتخزين البيانات بشكل منظم.  

---

## 🚀 المميزات
- 📝 إنشاء وحذف الملفات والمجلدات.  
- 📂 إدارة **Inodes** وتخزين الميتاداتا Metadata.  
- 🌳 استخدام **B-Tree** لإدارة الكتل الحرة (Free Blocks).  
- 💾 محاكاة قرص افتراضي (Virtual Disk) للقراءة والكتابة.  
- 🔒 دعم التحقق من السلامة باستخدام **Checksum**.  
- ⚡ دعم إلغاء التجزئة (Defragmentation).  
- 🖥️ واجهة أوامر (CLI) للتعامل مع الملفات.  

---

## 🏗️ مكونات النظام
- **VirtualDisk**: مسؤول عن إنشاء القرص الافتراضي، تخصيص وقراءة/كتابة الكتل.  
- **MiniHSFS**: الطبقة الأساسية للنظام تشمل Inodes, B-Tree, Superblock.  
- **Inode Table**: تخزن بيانات الملفات (الحجم، المستخدم، التوقيتات...).  
- **B-Tree**: لإدارة مواقع الكتل الحرة بكفاءة.  
- **Parser & Tokenizer**: لتحليل أوامر المستخدم (create, delete, read, write...).  

---

## ⚙️ المتطلبات
- C++17 أو أحدث.  
- CMake أو Visual Studio / g++ للتجميع.  

---

## ▶️ طريقة التشغيل
1. استنسخ المشروع:
   ```bash
   git clone https://github.com/username/MiniHSFS.git
   cd MiniHSFS
