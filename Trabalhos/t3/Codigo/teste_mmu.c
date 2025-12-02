// Arquivo de teste para diagnosticar problemas na MMU

#include "mmu.h"
#include "memoria.h"
#include "tabpag.h"
#include <stdio.h>

void teste_mmu_basico(void)
{
  printf("\n========== TESTE MMU BÁSICO ==========\n");
  
  // Cria memória e tabela de páginas
  mem_t *mem = mem_cria(1000);
  tabpag_t *tabpag = tabpag_cria();
  mmu_t *mmu = mmu_cria(mem);
  
  // Configura MMU
  mmu_define_tabpag(mmu, tabpag);
  
  // Mapeia página 0 no quadro 10
  int pagina = 0;
  int quadro = 10;
  tabpag_define_quadro(tabpag, pagina, quadro);
  
  printf("Mapeamento: página %d -> quadro %d\n", pagina, quadro);
  
  // Escreve na memória física
  int end_fis = quadro * TAM_PAGINA + 5;
  int valor_escrito = 42;
  err_t err = mem_escreve(mem, end_fis, valor_escrito);
  printf("Escrita física end=%d valor=%d err=%d\n", end_fis, valor_escrito, err);
  
  // Tenta ler via MMU (endereço virtual)
  int end_virt = pagina * TAM_PAGINA + 5;
  int valor_lido;
  err = mmu_le(mmu, end_virt, &valor_lido, usuario);
  printf("Leitura virtual end=%d valor=%d err=%d\n", end_virt, valor_lido, err);
  
  if (valor_lido == valor_escrito) {
    printf("✓ SUCESSO: valores coincidem!\n");
  } else {
    printf("✗ ERRO: esperado %d, obtido %d\n", valor_escrito, valor_lido);
  }
  
  // Cleanup
  mmu_destroi(mmu);
  tabpag_destroi(tabpag);
  mem_destroi(mem);
  
  printf("========== FIM TESTE ==========\n\n");
}

int main(void)
{
  teste_mmu_basico();
  return 0;
}
