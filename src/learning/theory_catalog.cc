#include "theory_catalog.h"

#include "learning/module_catalog.h"
#include "learning/skill_catalog.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace {

const std::vector<TheoryBlock> kTheoryBlocks{
    {
        .id = "storage_duration.basics",
        .skill_id = "core.storage_duration",
        .title = "Длительность хранения и существование объекта",
        .principle =
            "Длительность хранения определяет, как долго доступна память для объекта. Она не заменяет понятие "
            "времени жизни: в одной области памяти объекты могут последовательно начинать и завершать существование.",
        .mechanism =
            "Автоматическая длительность хранения обычно связана с входом в блок и выходом из него, статическая — "
            "с выполнением программы, потоковая — с выполнением потока, динамическая управляется выделением и "
            "освобождением памяти. Область видимости имени, доступность памяти и время жизни объекта — разные вещи.",
        .example = "const int* pointer;\n{\n  int value = 7;\n  pointer = &value;\n}\n// Память value больше нельзя "
                   "использовать через pointer.",
        .common_mistake =
            "Считать, что сохранённый адрес продлевает существование автоматического объекта или делает обращение к "
            "нему допустимым после выхода из блока.",
        .sources = {"https://eel.is/c++draft/basic.stc", "https://eel.is/c++draft/basic.life"},
    },
    {
        .id = "raii.scope_cleanup",
        .skill_id = "raii.fundamentals",
        .title = "RAII и освобождение ресурса при выходе из области",
        .principle =
            "RAII связывает владение ресурсом со временем жизни объекта: успешно созданный объект хранит ресурс, а "
            "его деструктор освобождает ресурс.",
        .mechanism =
            "Автоматические объекты уничтожаются при выходе из области, в том числе при раскрутке стека из-за "
            "исключения. Уже построенные подобъекты также уничтожаются, если последующее построение завершилось "
            "исключением. Поэтому освобождение должно находиться в деструкторе владеющего объекта, а не только в "
            "последней строке обычного пути выполнения.",
        .example = "void update() {\n  std::lock_guard guard(mutex);\n  change_state();\n}\n// mutex будет освобождён "
                   "и при обычном выходе, и при исключении.",
        .common_mistake =
            "Получить ресурс вручную, а освобождать отдельным вызовом, до которого выполнение может не дойти из-за "
            "раннего return или исключения.",
        .sources = {"https://eel.is/c++draft/class.dtor", "https://eel.is/c++draft/except.ctor",
                    "https://eel.is/c++draft/thread.lock.guard"},
    },
    {
        .id = "lifetime.references",
        .skill_id = "lifetime.objects",
        .title = "Время жизни объекта, указатели и ссылки",
        .principle =
            "Указатель, ссылка или представление без владения не продлевают время жизни объекта, на который они "
            "указывают. После завершения времени жизни прежнего объекта такой доступ нельзя считать допустимым.",
        .mechanism =
            "Время жизни начинается и заканчивается по правилам конкретного объекта. Для некоторых привязок ссылки "
            "стандарт отдельно продлевает время жизни временного объекта, но это не общее свойство ссылок. В "
            "частности, возврат ссылки из функции не продлевает жизнь локального или временного объекта.",
        .example = "const int* pointer;\n{\n  int value = 7;\n  pointer = &value;\n}\n// Разыменование pointer здесь "
                   "обращалось бы к объекту, время жизни которого завершилось.",
        .common_mistake =
            "Рассуждать только о том, что адрес или значение ссылки сохранились, и не проверять, существует ли ещё "
            "сам объект.",
        .sources = {"https://eel.is/c++draft/basic.life", "https://eel.is/c++draft/class.temporary"},
    },
    {
        .id = "smart_pointers.ownership",
        .skill_id = "lifetime.smart_pointers",
        .title = "Владение и умные указатели",
        .principle =
            "Тип умного указателя выражает модель владения: std::unique_ptr — единственный владелец, std::shared_ptr "
            "— совместное владение через общий управляющий блок, std::weak_ptr — наблюдение без продления жизни.",
        .mechanism =
            "Перемещение std::unique_ptr передаёт владение. Копии std::shared_ptr разделяют один управляющий блок; "
            "создавать независимые shared_ptr из одного сырого указателя нельзя. std::weak_ptr::lock() позволяет "
            "атомарно получить временного совместного владельца, если объект ещё существует.",
        .example =
            "auto owner = std::make_shared<Widget>();\nstd::weak_ptr<Widget> observer = owner;\nif (auto value = "
            "observer.lock()) {\n  value->use();\n}",
        .common_mistake =
            "Выбирать shared_ptr просто для удобства передачи указателя или создавать несколько управляющих блоков "
            "для одного объекта.",
        .sources = {"https://eel.is/c++draft/unique.ptr", "https://eel.is/c++draft/util.smartptr.shared",
                    "https://eel.is/c++draft/util.smartptr.weak"},
    },
};

}  // namespace

std::span<const TheoryBlock> TheoryCatalog() {
  return kTheoryBlocks;
}

const TheoryBlock* FindTheoryForSkill(std::string_view skill_id) {
  const auto it = std::ranges::find(kTheoryBlocks, skill_id, &TheoryBlock::skill_id);
  return it == kTheoryBlocks.end() ? nullptr : &*it;
}

void ValidateTheoryCatalog() {
  std::unordered_set<std::string_view> ids;
  std::unordered_set<std::string_view> skills;
  for (const auto& block : kTheoryBlocks) {
    if (block.id.empty() || block.skill_id.empty() || block.title.empty() || block.principle.empty() ||
        block.mechanism.empty() || block.example.empty() || block.common_mistake.empty() || block.sources.empty()) {
      throw std::logic_error("theory block fields must not be empty");
    }
    if (FindSkill(block.skill_id) == nullptr) {
      throw std::logic_error("theory block references unknown skill: " + std::string(block.skill_id));
    }
    if (!ids.insert(block.id).second) {
      throw std::logic_error("duplicate theory block id: " + std::string(block.id));
    }
    if (!skills.insert(block.skill_id).second) {
      throw std::logic_error("a skill has more than one primary theory block: " + std::string(block.skill_id));
    }
    for (const auto source : block.sources) {
      if (!source.starts_with("https://eel.is/c++draft/")) {
        throw std::logic_error("pilot theory must use a primary C++ draft source: " + std::string(block.id));
      }
    }
  }

  const auto* pilot = FindLearningModule("lifetime_raii");
  if (pilot == nullptr) {
    throw std::logic_error("the lifetime and RAII pilot module is missing");
  }
  for (const auto skill_id : pilot->skill_ids) {
    if (FindTheoryForSkill(skill_id) == nullptr) {
      throw std::logic_error("the pilot module has no theory block for skill: " + std::string(skill_id));
    }
  }
}
